#pragma once

#include <flash/receiver.h>

#include <obs-module.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

class VideoTrack;
class AudioTrack;

// Une connexion par émetteur, partagée par toutes les sources FLASH qui le visent.
// Chaque piste n'est décodée que si au moins une source l'utilise.
// Un émetteur : par son identifiant (adresse retrouvée par la découverte, clé d'appairage),
// ou par une adresse saisie à la main.
struct Target {
	std::string sender_id;
	std::string address;
	uint16_t port = 9710;
	int latency_ms = 10;
	std::string key() const { return sender_id.empty() ? address + ":" + std::to_string(port) : sender_id; }
};

class Connection {
public:
	static std::shared_ptr<Connection> get(const Target &target);
	~Connection();

	// Piste désignée par son nom (« Écran 1 », « discord »…), sinon par son numéro (à partir
	// de 1) parmi les vidéos ou parmi les sons de l'émetteur.
	void add_source(obs_source_t *source, bool video, int track, const std::string &name);
	void remove_source(obs_source_t *source);
	bool connected() const { return receiver_.connected(); }

	flash::ClockMap &clock() { return clock_; }
	void request_keyframe(uint16_t pid);

	// Écart moyen entre l'heure prévue d'une image et l'instant où OBS l'affiche :
	// le son s'y aligne pour rester synchro avec l'image réellement montrée.
	std::atomic<int64_t> phase_ns{0};
	// La piste vidéo qui fixe cette phase : la principale (PID le plus bas parmi celles reçues)
	std::atomic<uint16_t> phase_pid{0};
	static uint64_t video_interval_ns();
	// Temps de l'arrivée d'une image à son rendu par OBS, renvoyé à l'émetteur chaque seconde
	void displayed(uint64_t arrival_to_display_ns);

private:
	explicit Connection(const Target &target);
	bool refresh(flash::SrtOptions &o);

	void on_streams(const std::vector<flash::TsStream> &streams);
	void on_packet(const flash::MediaPacket &pkt);
	void on_state(bool connected);
	void rebind();
	void on_meta(std::span<const uint8_t> data);

	struct Subscription {
		obs_source_t *source;
		bool video;
		int track;
		std::string name;
	};

	Target target_;
	std::string label_;
	int failures_ = 0;
	flash::Receiver receiver_;
	flash::ClockMap clock_;

	std::mutex mutex_;
	std::vector<flash::TsStream> streams_;
	std::map<uint16_t, std::string> names_; // nom de chaque piste, d'après les métadonnées
	std::vector<Subscription> subs_;
	std::map<uint16_t, std::unique_ptr<VideoTrack>> video_;
	std::map<uint16_t, std::unique_ptr<AudioTrack>> audio_;
	std::atomic<uint64_t> last_keyframe_request_{0};
	uint64_t last_report_ = 0;
	std::string meta_;
	int64_t max_video_lag_ = INT64_MIN;
	int64_t max_audio_lag_ = INT64_MIN;
	std::atomic<uint64_t> display_sum_{0};
	std::atomic<uint64_t> display_count_{0};
};
