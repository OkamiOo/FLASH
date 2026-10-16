#include "flash/ts.h"

#include <cstring>

namespace flash {

void TsDemuxer::reset()
{
	for (PidState &st : pids_)
		st = PidState{};
	pmt_pid_ = 0;
	streams_.clear();
}

void TsDemuxer::push(const uint8_t *data, size_t size)
{
	for (size_t i = 0; i + ts::kPacket <= size; i += ts::kPacket) {
		if (data[i] == 0x47)
			packet(data + i);
	}
}

void TsDemuxer::packet(const uint8_t *p)
{
	const bool pusi = (p[1] & 0x40) != 0;
	const uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
	const uint8_t afc = (p[3] >> 4) & 0x03;
	const uint8_t cc = p[3] & 0x0F;

	size_t pos = 4;
	bool end_marker = false;
	bool random_access = false;
	if (afc & 0x02) {
		const size_t af_len = p[4];
		if (af_len > 183)
			return;
		if (af_len > 0) {
			const uint8_t flags = p[5];
			const size_t af_end = 5 + af_len;
			random_access = (flags & 0x40) != 0;
			size_t q = 6;
			if (flags & 0x10)
				q += 6; // PCR
			if (flags & 0x08)
				q += 6; // OPCR
			if (flags & 0x04)
				q += 1; // splice_countdown
			if ((flags & 0x02) && q < af_end) {
				const size_t plen = p[q];
				end_marker = plen >= 4 && q + 1 + 4 <= af_end && std::memcmp(p + q + 1, "FLSH", 4) == 0;
			}
		}
		pos = 5 + af_len;
	}
	if (!(afc & 0x01) || pos >= ts::kPacket)
		return;

	const uint8_t *payload = p + pos;
	const size_t n = ts::kPacket - pos;

	if (pid == ts::kPidPat || (pmt_pid_ && pid == pmt_pid_)) {
		if (pusi)
			section(pid, payload, n);
		return;
	}

	PidState &st = pids_[pid];
	if (!st.active)
		return;

	if (st.last_cc >= 0 && cc != ((st.last_cc + 1) & 0x0F)) {
		if (cc == st.last_cc)
			return; // paquet dupliqué
		st.corrupt = true;
	}
	st.last_cc = cc;

	if (pusi) {
		if (st.started && !st.buf.empty())
			finish(pid, st); // PES précédent sans marqueur ni longueur
		st.buf.clear();
		st.started = true;
		st.corrupt = false;
		st.expected = 0;
		st.random_access = random_access;
	} else if (!st.started) {
		return;
	}

	st.buf.insert(st.buf.end(), payload, payload + n);

	if (pusi && st.buf.size() >= 6) {
		const size_t len = ((size_t)st.buf[4] << 8) | st.buf[5];
		st.expected = len ? len + 6 : 0;
	}

	if (end_marker || (st.expected && st.buf.size() >= st.expected))
		finish(pid, st);
}

void TsDemuxer::finish(uint16_t pid, PidState &st)
{
	const std::vector<uint8_t> &b = st.buf;
	if (b.size() >= 9 && b[0] == 0 && b[1] == 0 && b[2] == 1) {
		const size_t header = 9 + (size_t)b[8];
		const size_t end = st.expected && st.expected < b.size() ? st.expected : b.size();
		if (header <= end) {
			TsPes pes;
			pes.pid = pid;
			pes.has_pts = (b[7] & 0x80) != 0 && b.size() >= 14;
			if (pes.has_pts) {
				pes.pts90k = ((uint64_t)(b[9] & 0x0E) << 29) | ((uint64_t)b[10] << 22) |
					     ((uint64_t)(b[11] & 0xFE) << 14) | ((uint64_t)b[12] << 7) | ((uint64_t)b[13] >> 1);
			}
			pes.data = b.data() + header;
			pes.size = end - header;
			pes.random_access = st.random_access;
			pes.corrupt = st.corrupt;
			if (on_pes)
				on_pes(pes);
		}
	}
	st.buf.clear();
	st.started = false;
	st.expected = 0;
	st.corrupt = false;
}

void TsDemuxer::section(uint16_t pid, const uint8_t *data, size_t size)
{
	const size_t pointer = data[0];
	if (1 + pointer + 3 > size)
		return;
	const uint8_t *s = data + 1 + pointer;
	const size_t avail = size - 1 - pointer;
	const size_t len = ((size_t)(s[1] & 0x0F) << 8) | s[2];
	if (3 + len > avail || len < 9 || crc32_mpeg(s, 3 + len) != 0)
		return;
	const size_t end = 3 + len - 4;

	if (pid == ts::kPidPat && s[0] == 0x00) {
		for (size_t i = 8; i + 4 <= end; i += 4) {
			const uint16_t program = (uint16_t)((s[i] << 8) | s[i + 1]);
			if (program != 0) {
				pmt_pid_ = (uint16_t)(((s[i + 2] & 0x1F) << 8) | s[i + 3]);
				break;
			}
		}
		return;
	}

	if (pid != pmt_pid_ || s[0] != 0x02 || end < 12)
		return;

	std::vector<TsStream> streams;
	size_t i = 12 + ((((size_t)s[10] & 0x0F) << 8) | s[11]);
	while (i + 5 <= end) {
		TsStream st;
		st.stream_type = s[i];
		st.pid = (uint16_t)(((s[i + 1] & 0x1F) << 8) | s[i + 2]);
		const size_t info_len = (((size_t)s[i + 3] & 0x0F) << 8) | s[i + 4];
		const uint8_t *d = s + i + 5;
		const uint8_t *d_end = d + info_len;
		if (d_end > s + end)
			break;

		if (st.stream_type == 0x1B) {
			st.codec = Codec::H264;
		} else if (st.stream_type == 0x24) {
			st.codec = Codec::HEVC;
		} else {
			st.codec = Codec::Meta;
			for (const uint8_t *q = d; q + 2 <= d_end; q += 2 + q[1]) {
				if (q[0] == 0x05 && q[1] >= 4 && q + 6 <= d_end) {
					if (std::memcmp(q + 2, "Opus", 4) == 0)
						st.codec = Codec::Opus;
				}
			}
		}
		streams.push_back(st);
		i += 5 + info_len;
	}

	bool changed = streams.size() != streams_.size();
	for (size_t k = 0; !changed && k < streams.size(); k++)
		changed = streams[k].pid != streams_[k].pid || streams[k].codec != streams_[k].codec;
	if (!changed)
		return;

	for (const TsStream &old : streams_)
		pids_[old.pid].active = false;
	for (const TsStream &st : streams)
		pids_[st.pid].active = true;
	streams_ = streams;
	if (on_streams)
		on_streams(streams_);
}

bool split_opus_pes(const uint8_t *data, size_t size, std::vector<std::span<const uint8_t>> &packets)
{
	size_t pos = 0;
	while (pos + 2 <= size) {
		if (data[pos] != 0x7F || (data[pos + 1] & 0xE0) != 0xE0)
			return false;
		const bool start_trim = (data[pos + 1] & 0x10) != 0;
		const bool end_trim = (data[pos + 1] & 0x08) != 0;
		const bool extension = (data[pos + 1] & 0x04) != 0;
		pos += 2;

		size_t au = 0;
		while (pos < size) {
			const uint8_t b = data[pos++];
			au += b;
			if (b != 0xFF)
				break;
		}
		if (start_trim)
			pos += 2;
		if (end_trim)
			pos += 2;
		if (extension && pos < size)
			pos += 1 + data[pos];
		if (pos + au > size)
			return false;
		packets.emplace_back(data + pos, au);
		pos += au;
	}
	return true;
}

} // namespace flash
