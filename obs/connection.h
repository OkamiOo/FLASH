#pragma once

#include <flash/receiver.h>

#include <obs-module.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class VideoTrack;
class AudioTrack;

// Une connexion par émetteur, partagée par toutes les sources FLASH qui le visent.
// Chaque piste n'est décodée que si au moins une source l'utilise.
class Connection {
public:
	static std::shared_ptr<Connection> get(const std::string &address, uint16_t port);
	~Connection();

	// piste : numéro (à partir de 1) parmi les vidéos ou parmi les sons de l'émetteur
	void add_source(obs_source_t *source, bool video, int track);
	void remove_source(obs_source_t *source);

	flash::ClockMap &clock() { return clock_; }
	void request_keyframe(uint16_t pid);

	// Écart moyen entre l'heure prévue d'une image et l'instant où OBS l'affiche :
	// le son s'y aligne pour rester synchro avec l'image réellement montrée.
	std::atomic<int64_t> phase_ns{0};
	static uint64_t video_interval_ns();

private:
	Connection(std::string address, uint16_t port);

	void on_streams(const std::vector<flash::TsStream> &streams);
	void on_packet(const flash::MediaPacket &pkt);
	void on_state(bool connected);
	void rebind();

	struct Subscription {
		obs_source_t *source;
		bool video;
		int track;
	};

	std::string address_;
	uint16_t port_;
	flash::Receiver receiver_;
	flash::ClockMap clock_;

	std::mutex mutex_;
	std::vector<flash::TsStream> streams_;
	std::vector<Subscription> subs_;
	std::map<uint16_t, std::unique_ptr<VideoTrack>> video_;
	std::map<uint16_t, std::unique_ptr<AudioTrack>> audio_;
	std::atomic<uint64_t> last_keyframe_request_{0};
	uint64_t last_report_ = 0;
	int64_t max_video_lag_ = INT64_MIN;
	int64_t max_audio_lag_ = INT64_MIN;
};
