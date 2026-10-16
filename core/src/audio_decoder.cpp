#include "flash/audio_decoder.h"
#include "flash/log.h"

#include <opus.h>

namespace flash {

AudioDecoder::~AudioDecoder()
{
	if (dec_)
		opus_decoder_destroy(dec_);
}

bool AudioDecoder::open()
{
	int err = 0;
	dec_ = opus_decoder_create(48000, 2, &err);
	if (err != OPUS_OK || !dec_) {
		FLOG_E("Ouverture du décodeur Opus impossible (%d)", err);
		dec_ = nullptr;
		return false;
	}
	return true;
}

int AudioDecoder::decode(const uint8_t *data, size_t size, float *pcm, int max_frames)
{
	if (!dec_)
		return -1;
	const int n = opus_decode_float(dec_, data, (opus_int32)size, pcm, max_frames, 0);
	return n > 0 ? n : -1;
}

} // namespace flash
