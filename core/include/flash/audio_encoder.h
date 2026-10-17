#pragma once

#include <cstdint>
#include <vector>

struct OpusEncoder;

namespace flash {

// Opus 48 kHz, trames de 5 ou 10 ms, mode basse latence (CELT seul, 2,5 ms de retard).
class AudioEncoder {
public:
	~AudioEncoder();

	bool open(int channels, int bitrate, int frame_ms = 10);
	// Encode une trame de frame_size() échantillons entrelacés.
	bool encode(const float *samples, std::vector<uint8_t> &out);
	int frame_size() const { return frame_size_; }
	int lookahead() const { return lookahead_; } // retard de l'encodeur, en échantillons

private:
	OpusEncoder *enc_ = nullptr;
	int frame_size_ = 480;
	int lookahead_ = 0;
};

} // namespace flash
