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
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
class Connection;

// Une piste vidéo, en deux fils :
// - le décodage, sur le GPU (d3d11va), puis copie de l'image en mémoire ;
// - la remise à OBS, chaque image à son heure.
// Les séparer garde la mesure du temps de décodage indépendante des attentes de remise.
class VideoTrack {
public:
	VideoTrack(Connection &conn, uint16_t pid, flash::Codec codec);
	~VideoTrack();

	void push(const flash::MediaPacket &pkt);
	void set_sources(std::vector<obs_source_t *> sources);
	flash::Codec codec() const { return codec_; }

private:
	struct Packet {
		uint64_t pts_ns;
		uint64_t arrival_ns;
		std::vector<uint8_t> data;
		bool keyframe;
		bool corrupt;
	};

	struct Ready {
		AVFrame *frame;
		uint64_t pts_ns;
		uint64_t arrival_ns;
		uint64_t decoded_ns;
		uint64_t target_ns;
		uint64_t release_ns;
	};

	void decode_loop();
	void release_loop();
	bool open_decoder();
	void close_decoder();
	void decode(const Packet &pkt);
	void schedule(AVFrame *frame, const Packet &pkt, uint64_t decoded_ns);
	void output(const Ready &r);
	AVFrame *take_frame();
	void give_frame(AVFrame *frame);

	Connection &conn_;
	const uint16_t pid_;
	const flash::Codec codec_;
	std::atomic<bool> stop_{false};

	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<Packet> queue_;

	std::mutex ready_mutex_;
	std::condition_variable ready_cv_;
	std::deque<Ready> ready_;
	std::vector<AVFrame *> pool_;

	std::mutex sources_mutex_;
	std::vector<obs_source_t *> sources_;

	std::thread decode_thread_;
	std::thread release_thread_;

	AVCodecContext *ctx_ = nullptr;
	AVBufferRef *hw_device_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	bool waiting_keyframe_ = true;
	flash::CadenceSmoother cadence_;
	int64_t prev_d_ = 0; // écart rendu - heure visée de l'image précédente
	bool have_prev_ = false;
	uint64_t frames_ = 0;
	uint64_t decoded_ = 0;
};
