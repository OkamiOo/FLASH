#pragma once

#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Opus 48 kHz, trames de 10 ms, mode basse latence.
class OpusEncoder {
public:
	~OpusEncoder();

	bool open(int channels, int bitrate);
	// Encode une trame de 480 échantillons entrelacés.
	bool encode(const float *samples, std::vector<uint8_t> &out);
	int frame_size() const { return 480; }
	int lookahead() const { return lookahead_; } // retard de l'encodeur, en échantillons

private:
	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	int channels_ = 2;
	int lookahead_ = 0;
	int64_t pts_ = 0;
};
