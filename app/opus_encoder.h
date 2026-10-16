#pragma once

#include <cstdint>
#include <vector>

struct OpusEncoder;

// Opus 48 kHz, trames de 10 ms, mode basse latence (CELT seul, 2,5 ms de retard).
class AudioEncoder {
public:
	~AudioEncoder();

	bool open(int channels, int bitrate);
	// Encode une trame de 480 échantillons entrelacés.
	bool encode(const float *samples, std::vector<uint8_t> &out);
	int frame_size() const { return 480; }
	int lookahead() const { return lookahead_; } // retard de l'encodeur, en échantillons

private:
	OpusEncoder *enc_ = nullptr;
	int lookahead_ = 0;
};
