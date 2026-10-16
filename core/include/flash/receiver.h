#pragma once

#include "flash/srt_link.h"
#include "flash/ts.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace flash {

// Messages de la voie de retour (récepteur vers émetteur)
namespace control {
constexpr uint8_t kKeyframeRequest = 0x01; // suivi du PID sur 2 octets
}

// Ramène l'horloge de l'émetteur sur celle du récepteur. Un seul modèle par émetteur :
// toutes ses pistes passent par le même décalage et restent donc alignées entre elles.
//
// Chaque observation donne le moment où un média horodaté pts est prêt à être présenté
// (reçu, et décodé pour la vidéo). Le décalage suit la 3e plus grande valeur de
// (prêt - pts) sur 2 s, plus une marge : un pic isolé est ignoré, un retard qui dure fait
// reculer tout de suite, et la descente reste lente pour suivre la dérive entre deux PC
// sans à-coups.
class ClockMap {
public:
	explicit ClockMap(uint64_t margin_ns = 2'000'000) : margin_(margin_ns) {}

	void observe(uint64_t pts_ns, uint64_t ready_ns);
	bool ready() const;
	uint64_t present(uint64_t pts_ns) const; // heure locale de présentation
	int64_t offset_ns() const;
	void set_margin(uint64_t margin_ns);
	void reset();

private:
	mutable std::mutex mutex_;
	std::deque<std::pair<uint64_t, int64_t>> window_; // (heure locale, prêt - pts)
	std::vector<int64_t> scratch_;
	int64_t offset_ = 0;
	uint64_t first_ns_ = 0;
	uint64_t start_ns_ = 0;
	uint64_t last_eval_ns_ = 0;
	uint64_t last_update_ns_ = 0;
	uint64_t margin_;
	bool have_offset_ = false;
};

struct MediaPacket {
	uint16_t pid = 0;
	Codec codec = Codec::Meta;
	uint64_t pts_ns = 0;     // horloge de l'émetteur
	uint64_t arrival_ns = 0; // horloge locale
	std::span<const uint8_t> data;
	bool keyframe = false;
	bool corrupt = false;
};

// Une connexion vers un émetteur : SRT, démultiplexage, horodatages sur 64 bits.
class Receiver {
public:
	using OnStreams = std::function<void(const std::vector<TsStream> &)>;
	using OnPacket = std::function<void(const MediaPacket &)>;
	using OnState = std::function<void(bool connected)>;

	~Receiver();

	void start(const SrtOptions &options, OnStreams on_streams, OnPacket on_packet, OnState on_state);
	void stop();

	void request_keyframe(uint16_t pid);
	SrtLinkStats stats() { return client_.stats(); }
	bool connected() const { return client_.connected(); }

private:
	uint64_t unwrap(uint64_t pts90k);

	SrtClient client_;
	TsDemuxer demux_;
	std::vector<TsStream> streams_;
	OnStreams on_streams_;
	OnPacket on_packet_;
	OnState on_state_;
	uint64_t arrival_ = 0;
	uint64_t last_pts_ = 0;
	bool have_pts_ = false;
};

} // namespace flash
