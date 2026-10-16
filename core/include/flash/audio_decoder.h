#pragma once

#include <cstddef>
#include <cstdint>

struct OpusDecoder;

namespace flash {

// Opus 48 kHz stéréo, en flottants entrelacés.
class AudioDecoder {
public:
	~AudioDecoder();

	bool open();
	// Décode un paquet ; rend le nombre d'échantillons par canal, ou -1.
	int decode(const uint8_t *data, size_t size, float *pcm, int max_frames);

private:
	OpusDecoder *dec_ = nullptr;
};

} // namespace flash
