#pragma once

#include <flash/receiver.h>

#include <obs-module.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
class Connection;

// Une piste son : décodage Opus dans son propre fil, puis remise à OBS sur l'horloge commune.
//
// OBS lisse les écarts d'horodatage de moins de 70 ms : il colle chaque bloc au précédent.
// On suit donc la chronologie qu'il applique réellement et on la ramène vers l'heure
// voulue en rééchantillonnant très légèrement (0,2 % au plus), sans saut ni craquement.
class AudioTrack {
public:
	AudioTrack(Connection &conn, uint16_t pid);
	~AudioTrack();

	void push(const flash::MediaPacket &pkt);
	void set_sources(std::vector<obs_source_t *> sources);

private:
	struct Packet {
		uint64_t pts_ns;
		std::vector<uint8_t> data;
	};

	void run();
	bool open();
	void decode(const Packet &pkt);
	void output(const AVFrame *frame, uint64_t intended_ns);
	void detect_beep(const float *left, uint32_t frames, uint64_t first_ns);

	Connection &conn_;
	const uint16_t pid_;

	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<Packet> queue_;
	std::atomic<bool> stop_{false};
	std::thread thread_;

	std::mutex mutex_;
	std::vector<obs_source_t *> sources_;

	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	SwrContext *swr_ = nullptr;
	uint32_t out_rate_ = 48000;
	std::vector<float> out_[2];

	bool timeline_ = false;
	uint64_t next_ts_ = 0; // horodatage qu'OBS donnera à notre prochain échantillon

	uint64_t quiet_samples_ = 0;
	bool in_beep_ = false;
};
