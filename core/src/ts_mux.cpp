#include "flash/ts.h"
#include "flash/clock.h"

#include <algorithm>
#include <cstring>

namespace flash {

namespace {

constexpr uint8_t kEndMarker[4] = {'F', 'L', 'S', 'H'};

uint8_t stream_type_of(Codec codec)
{
	switch (codec) {
	case Codec::H264:
		return 0x1B;
	case Codec::HEVC:
		return 0x24;
	default:
		return 0x06; // PES privé (Opus, AV1, métadonnées)
	}
}

void put_pts(uint8_t *p, uint64_t pts)
{
	pts &= 0x1FFFFFFFFULL;
	p[0] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
	p[1] = (uint8_t)(pts >> 22);
	p[2] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
	p[3] = (uint8_t)(pts >> 7);
	p[4] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
}

void put16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back((uint8_t)(x >> 8));
	v.push_back((uint8_t)x);
}

void finish_section(std::vector<uint8_t> &s)
{
	// section_length : tout ce qui suit le champ, CRC compris
	const size_t len = s.size() - 3 + 4;
	s[1] = (uint8_t)((s[1] & 0xF0) | ((len >> 8) & 0x0F));
	s[2] = (uint8_t)len;
	const uint32_t crc = crc32_mpeg(s.data(), s.size());
	s.push_back((uint8_t)(crc >> 24));
	s.push_back((uint8_t)(crc >> 16));
	s.push_back((uint8_t)(crc >> 8));
	s.push_back((uint8_t)crc);
}

} // namespace

TsMuxer::TsMuxer(Sink sink) : sink_(std::move(sink)) {}

void TsMuxer::set_tracks(const std::vector<Track> &tracks)
{
	std::lock_guard lock(mutex_);
	tracks_ = tracks;
	pmt_version_ = (uint8_t)((pmt_version_ + 1) & 0x1F);
}

void TsMuxer::write_tables()
{
	std::lock_guard lock(mutex_);

	std::vector<uint8_t> pat = {0x00, 0xB0, 0x00};
	put16(pat, 1); // transport_stream_id
	pat.push_back(0xC1);
	pat.push_back(0x00);
	pat.push_back(0x00);
	put16(pat, 1); // programme 1
	put16(pat, (uint16_t)(0xE000 | ts::kPidPmt));
	finish_section(pat);
	write_section(ts::kPidPat, pat);

	std::vector<uint8_t> pmt = {0x02, 0xB0, 0x00};
	put16(pmt, 1);
	pmt.push_back((uint8_t)(0xC1 | (pmt_version_ << 1)));
	pmt.push_back(0x00);
	pmt.push_back(0x00);
	put16(pmt, (uint16_t)(0xE000 | ts::kPidPcr));
	put16(pmt, 0xF000); // pas de descripteur de programme

	auto add_stream = [&](uint8_t type, uint16_t pid, const std::vector<uint8_t> &desc) {
		pmt.push_back(type);
		put16(pmt, (uint16_t)(0xE000 | pid));
		put16(pmt, (uint16_t)(0xF000 | desc.size()));
		pmt.insert(pmt.end(), desc.begin(), desc.end());
	};

	for (const Track &t : tracks_) {
		std::vector<uint8_t> desc;
		if (t.codec == Codec::Opus)
			desc = {0x05, 4, 'O', 'p', 'u', 's', 0x7F, 2, 0x80, t.channels};
		else if (t.codec == Codec::AV1)
			desc = {0x05, 4, 'A', 'V', '0', '1'};
		add_stream(stream_type_of(t.codec), t.pid, desc);
	}
	add_stream(0x06, ts::kPidMeta, {0x05, 4, 'F', 'L', 'S', 'H'});

	finish_section(pmt);
	write_section(ts::kPidPmt, pmt);
	flush();
}

void TsMuxer::write_pcr(uint64_t now)
{
	std::lock_guard lock(mutex_);

	const uint64_t t = now > ts::kPcrDelayNs ? now - ts::kPcrDelayNs : 0;
	// 27 MHz : base en 90 kHz, extension sur 300
	const uint64_t pcr27 = (t / 1000) * 27 + (t % 1000) * 27 / 1000;
	const uint64_t base = (pcr27 / 300) & 0x1FFFFFFFFULL;
	const uint32_t ext = (uint32_t)(pcr27 % 300);

	uint8_t p[ts::kPacket];
	p[0] = 0x47;
	p[1] = (uint8_t)(ts::kPidPcr >> 8);
	p[2] = (uint8_t)ts::kPidPcr;
	p[3] = (uint8_t)(0x20 | (cc_[ts::kPidPcr] & 0x0F)); // champ d'adaptation seul : compteur inchangé
	p[4] = 183;
	p[5] = 0x10;
	p[6] = (uint8_t)(base >> 25);
	p[7] = (uint8_t)(base >> 17);
	p[8] = (uint8_t)(base >> 9);
	p[9] = (uint8_t)(base >> 1);
	p[10] = (uint8_t)(((base & 1) << 7) | 0x7E | (ext >> 8));
	p[11] = (uint8_t)ext;
	std::memset(p + 12, 0xFF, ts::kPacket - 12);
	push_packet(p);
	flush();
}

void TsMuxer::write_video(uint16_t pid, uint64_t pts_ns, const uint8_t *data, size_t size, bool keyframe)
{
	std::lock_guard lock(mutex_);
	// L'AV1, en PES privé, prend le stream_id des données privées
	uint8_t stream_id = 0xE0;
	for (const Track &t : tracks_)
		if (t.pid == pid && t.codec == Codec::AV1)
			stream_id = 0xBD;
	write_pes(pid, stream_id, pts_ns, {}, {data, size}, keyframe, true);
	flush();
}

void TsMuxer::write_audio(uint16_t pid, uint64_t pts_ns, const uint8_t *data, size_t size)
{
	std::lock_guard lock(mutex_);

	// En-tête de contrôle Opus pour le TS : 0x7FE0 puis la taille codée en base 255
	uint8_t prefix[2 + 16];
	size_t n = 0;
	prefix[n++] = 0x7F;
	prefix[n++] = 0xE0;
	size_t s = size;
	while (s >= 255 && n < sizeof(prefix) - 1) {
		prefix[n++] = 0xFF;
		s -= 255;
	}
	prefix[n++] = (uint8_t)s;

	write_pes(pid, 0xBD, pts_ns, {prefix, n}, {data, size}, true, false);
	flush();
}

void TsMuxer::write_meta(const std::string &text)
{
	std::lock_guard lock(mutex_);
	write_pes(ts::kPidMeta, 0xBD, now_ns(), {}, {(const uint8_t *)text.data(), text.size()}, true, false);
	flush();
}

void TsMuxer::write_pes(uint16_t pid, uint8_t stream_id, uint64_t pts_ns, std::span<const uint8_t> prefix,
			std::span<const uint8_t> payload, bool random_access, bool mark_end)
{
	const size_t body = 3 + 5 + prefix.size() + payload.size();

	pes_.resize(6 + body);
	uint8_t *p = pes_.data();
	p[0] = 0x00;
	p[1] = 0x00;
	p[2] = 0x01;
	p[3] = stream_id;
	const size_t len = body <= 0xFFFF ? body : 0; // 0 = longueur non bornée (image vidéo, close par le marqueur)
	p[4] = (uint8_t)(len >> 8);
	p[5] = (uint8_t)len;
	p[6] = 0x84; // data_alignment_indicator
	p[7] = 0x80; // PTS seul (pas de B-frames : DTS = PTS)
	p[8] = 5;
	put_pts(p + 9, ns_to_90k(pts_ns));
	if (!prefix.empty())
		std::memcpy(p + 14, prefix.data(), prefix.size());
	std::memcpy(p + 14 + prefix.size(), payload.data(), payload.size());

	// Découpe en paquets. Le dernier paquet d'un PES marqué porte le marqueur de fin dans
	// son champ d'adaptation, ce qui laisse au plus 177 octets de charge utile.
	constexpr size_t kLastCap = ts::kPacket - 4 - 7;
	size_t pos = 0;
	bool first = true;
	const size_t total = pes_.size();
	while (pos < total) {
		const size_t rem = total - pos;
		const uint8_t flags = (first && random_access) ? 0x40 : 0x00;

		if (mark_end ? rem <= kLastCap : rem <= ts::kPacket - 4 - (flags ? 2 : 0)) {
			emit_packet(pid, first, flags, mark_end ? kEndMarker : nullptr, mark_end ? sizeof(kEndMarker) : 0,
				    p + pos, rem);
			break;
		}

		const size_t cap = ts::kPacket - 4 - (flags ? 2 : 0);
		size_t take = rem < cap ? rem : cap;
		if (take == rem)
			take = rem - 1; // garde au moins un octet pour le paquet marqué
		emit_packet(pid, first, flags, nullptr, 0, p + pos, take);
		pos += take;
		first = false;
	}
}

// Une table qui ne tient pas dans un paquet (beaucoup de pistes) continue dans les suivants
void TsMuxer::write_section(uint16_t pid, const std::vector<uint8_t> &section)
{
	size_t pos = 0;
	for (bool first = true; first || pos < section.size(); first = false) {
		uint8_t p[ts::kPacket];
		p[0] = 0x47;
		p[1] = (uint8_t)((first ? 0x40 : 0x00) | (pid >> 8));
		p[2] = (uint8_t)pid;
		p[3] = (uint8_t)(0x10 | (cc_[pid] & 0x0F));
		cc_[pid] = (uint8_t)((cc_[pid] + 1) & 0x0F);
		size_t at = 4;
		if (first)
			p[at++] = 0x00; // pointer_field
		const size_t n = std::min(section.size() - pos, ts::kPacket - at);
		std::memcpy(p + at, section.data() + pos, n);
		std::memset(p + at + n, 0xFF, ts::kPacket - at - n);
		push_packet(p);
		pos += n;
	}
}

void TsMuxer::emit_packet(uint16_t pid, bool pusi, uint8_t af_flags, const uint8_t *priv, size_t priv_size,
			  const uint8_t *payload, size_t payload_size)
{
	uint8_t p[ts::kPacket];
	p[0] = 0x47;
	p[1] = (uint8_t)((pusi ? 0x40 : 0x00) | (pid >> 8));
	p[2] = (uint8_t)pid;

	const bool need_af = af_flags || priv || payload_size < ts::kPacket - 4;
	p[3] = (uint8_t)((need_af ? 0x30 : 0x10) | (cc_[pid] & 0x0F));
	cc_[pid] = (uint8_t)((cc_[pid] + 1) & 0x0F);

	size_t pos = 4;
	if (need_af) {
		const size_t af_body = ts::kPacket - 5 - payload_size; // octets après la longueur
		p[pos++] = (uint8_t)af_body;
		if (af_body > 0) {
			size_t written = 1;
			p[pos++] = (uint8_t)(af_flags | (priv ? 0x02 : 0x00));
			if (priv) {
				p[pos++] = (uint8_t)priv_size;
				std::memcpy(p + pos, priv, priv_size);
				pos += priv_size;
				written += 1 + priv_size;
			}
			std::memset(p + pos, 0xFF, af_body - written);
			pos += af_body - written;
		}
	}
	std::memcpy(p + pos, payload, payload_size);
	push_packet(p);
}

void TsMuxer::push_packet(const uint8_t *packet)
{
	std::memcpy(msg_.data() + msg_size_, packet, ts::kPacket);
	msg_size_ += ts::kPacket;
	if (msg_size_ == msg_.size())
		flush();
}

void TsMuxer::flush()
{
	if (msg_size_) {
		sink_(msg_.data(), msg_size_);
		msg_size_ = 0;
	}
}

} // namespace flash
