#include "flash/audio_encoder.h"
#include "flash/log.h"

#include <opus.h>

namespace flash {

AudioEncoder::~AudioEncoder()
{
	if (enc_)
		opus_encoder_destroy(enc_);
}

bool AudioEncoder::open(int channels, int bitrate)
{
	int err = 0;
	enc_ = opus_encoder_create(48000, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
	if (err != OPUS_OK || !enc_) {
		FLOG_E("Ouverture de l'encodeur Opus impossible (%d)", err);
		enc_ = nullptr;
		return false;
	}
	opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(10));
	opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&lookahead_));
	return true;
}

bool AudioEncoder::encode(const float *samples, std::vector<uint8_t> &out)
{
	out.resize(1500);
	const opus_int32 n = opus_encode_float(enc_, samples, frame_size(), out.data(), (opus_int32)out.size());
	if (n <= 0) {
		out.clear();
		return false;
	}
	out.resize((size_t)n);
	return true;
}

} // namespace flash
