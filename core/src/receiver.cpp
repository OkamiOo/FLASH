#include "flash/receiver.h"
#include "flash/clock.h"
#include "flash/log.h"

#include <algorithm>
#include <functional>

namespace flash {

namespace {
constexpr uint64_t kWindowNs = 2'000'000'000ULL;   // fenêtre d'observation
constexpr size_t kRank = 3;                        // 3e plus grande valeur : un pic isolé ne compte pas
constexpr uint64_t kEvalNs = 50'000'000ULL;        // réévaluation toutes les 50 ms
constexpr uint64_t kWarmupNs = 500'000'000ULL;     // observations avant de présenter quoi que ce soit
constexpr int64_t kSlewDownNsPerS = 100'000;       // descente : 0,1 ms par seconde au plus...
constexpr int64_t kFastSlewNsPerS = 2'000'000;     // ...sauf au démarrage : 2 ms par seconde
constexpr uint64_t kFastPeriodNs = 10'000'000'000ULL;
constexpr uint64_t kMask33 = (1ULL << 33) - 1;
} // namespace

// ---------- ClockMap ----------

void ClockMap::observe(uint64_t pts_ns, uint64_t ready_ns)
{
	std::lock_guard lock(mutex_);
	if (!first_ns_)
		first_ns_ = ready_ns;

	window_.emplace_back(ready_ns, (int64_t)(ready_ns - pts_ns));
	while (window_.front().first + kWindowNs < ready_ns)
		window_.pop_front();
	if (ready_ns - last_eval_ns_ < kEvalNs)
		return;
	last_eval_ns_ = ready_ns;

	scratch_.clear();
	for (const auto &[t, lag] : window_)
		scratch_.push_back(lag);
	const size_t k = std::min(kRank, scratch_.size());
	std::nth_element(scratch_.begin(), scratch_.begin() + (k - 1), scratch_.end(), std::greater<int64_t>());
	const int64_t target = scratch_[k - 1] + (int64_t)margin_;

	if (!have_offset_) {
		if (ready_ns - first_ns_ >= kWarmupNs) {
			offset_ = target;
			have_offset_ = true;
			start_ns_ = ready_ns;
			last_update_ns_ = ready_ns;
		}
		return;
	}

	if (target > offset_) {
		offset_ = target; // en retard : on recule tout de suite
	} else if (target < offset_) {
		const uint64_t dt = ready_ns - last_update_ns_;
		const int64_t rate = ready_ns - start_ns_ < kFastPeriodNs ? kFastSlewNsPerS : kSlewDownNsPerS;
		const int64_t step = (int64_t)(dt * rate / 1'000'000'000ULL);
		offset_ = target > offset_ - step ? target : offset_ - step;
	}
	last_update_ns_ = ready_ns;
}

bool ClockMap::ready() const
{
	std::lock_guard lock(mutex_);
	return have_offset_;
}

uint64_t ClockMap::present(uint64_t pts_ns) const
{
	std::lock_guard lock(mutex_);
	return pts_ns + (uint64_t)offset_;
}

int64_t ClockMap::offset_ns() const
{
	std::lock_guard lock(mutex_);
	return offset_;
}

void ClockMap::set_margin(uint64_t margin_ns)
{
	std::lock_guard lock(mutex_);
	if (have_offset_)
		offset_ += (int64_t)margin_ns - (int64_t)margin_;
	margin_ = margin_ns;
}

void ClockMap::reset()
{
	std::lock_guard lock(mutex_);
	window_.clear();
	offset_ = 0;
	first_ns_ = 0;
	start_ns_ = 0;
	last_eval_ns_ = 0;
	last_update_ns_ = 0;
	have_offset_ = false;
}

// ---------- Receiver ----------

Receiver::~Receiver()
{
	stop();
}

void Receiver::start(const SrtOptions &options, OnStreams on_streams, OnPacket on_packet, OnState on_state)
{
	on_streams_ = std::move(on_streams);
	on_packet_ = std::move(on_packet);
	on_state_ = std::move(on_state);

	demux_.on_streams = [this](const std::vector<TsStream> &streams) {
		streams_ = streams;
		if (on_streams_)
			on_streams_(streams);
	};

	demux_.on_pes = [this](const TsPes &pes) {
		if (!pes.has_pts)
			return;
		MediaPacket pkt;
		pkt.pid = pes.pid;
		for (const TsStream &s : streams_) {
			if (s.pid == pes.pid)
				pkt.codec = s.codec;
		}
		pkt.pts_ns = ts90k_to_ns(unwrap(pes.pts90k));
		pkt.arrival_ns = arrival_;
		pkt.data = {pes.data, pes.size};
		pkt.keyframe = pes.random_access;
		pkt.corrupt = pes.corrupt;
		on_packet_(pkt);
	};

	srt_init(false);
	client_.start(
		options, [this](const uint8_t *data, size_t size, uint64_t arrival) {
			arrival_ = arrival;
			demux_.push(data, size);
		},
		[this](bool connected) {
			// Nouvelle connexion : l'émetteur a pu redémarrer, on repart de zéro
			demux_.reset();
			streams_.clear();
			have_pts_ = false;
			if (on_state_)
				on_state_(connected);
		});
}

void Receiver::stop()
{
	client_.stop();
}

void Receiver::request_keyframe(uint16_t pid)
{
	const uint8_t msg[3] = {control::kKeyframeRequest, (uint8_t)(pid >> 8), (uint8_t)pid};
	client_.send(msg, sizeof(msg));
}

uint64_t Receiver::unwrap(uint64_t pts)
{
	if (!have_pts_) {
		// Premier horodatage : on le place au plus près de l'heure locale. Sur un même PC,
		// on retrouve ainsi l'heure exacte de l'émetteur ; entre deux PC, la base est
		// arbitraire mais seule compte la continuité.
		const uint64_t ref = ns_to_90k(now_ns());
		uint64_t ext = (ref & ~kMask33) | (pts & kMask33);
		if (ext > ref + (1ULL << 32))
			ext -= 1ULL << 33;
		else if (ext + (1ULL << 32) < ref)
			ext += 1ULL << 33;
		last_pts_ = ext;
		have_pts_ = true;
		return ext;
	}

	int64_t d = (int64_t)((pts - (last_pts_ & kMask33)) & kMask33);
	if (d >= (int64_t)(1ULL << 32))
		d -= (int64_t)(1ULL << 33);
	last_pts_ += (uint64_t)d;
	return last_pts_;
}

} // namespace flash
