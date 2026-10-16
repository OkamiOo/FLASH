#include "opus_encoder.h"

#include <flash/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <cstring>

OpusEncoder::~OpusEncoder()
{
	av_packet_free(&packet_);
	av_frame_free(&frame_);
	avcodec_free_context(&ctx_);
}

bool OpusEncoder::open(int channels, int bitrate)
{
	const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
	if (!codec) {
		FLOG_E("Encodeur Opus introuvable");
		return false;
	}

	channels_ = channels;
	ctx_ = avcodec_alloc_context3(codec);
	ctx_->sample_rate = 48000;
	ctx_->sample_fmt = AV_SAMPLE_FMT_FLT;
	ctx_->bit_rate = bitrate;
	av_channel_layout_default(&ctx_->ch_layout, channels);
	av_opt_set(ctx_->priv_data, "application", "lowdelay", 0);
	av_opt_set_double(ctx_->priv_data, "frame_duration", 10.0, 0);

	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		FLOG_E("Ouverture de l'encodeur Opus impossible");
		return false;
	}
	lookahead_ = ctx_->initial_padding;

	frame_ = av_frame_alloc();
	frame_->nb_samples = frame_size();
	frame_->format = AV_SAMPLE_FMT_FLT;
	frame_->sample_rate = 48000;
	av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
	av_frame_get_buffer(frame_, 0);
	packet_ = av_packet_alloc();
	return true;
}

bool OpusEncoder::encode(const float *samples, std::vector<uint8_t> &out)
{
	out.clear();
	if (av_frame_make_writable(frame_) < 0)
		return false;
	std::memcpy(frame_->data[0], samples, sizeof(float) * frame_size() * channels_);
	frame_->pts = pts_;
	pts_ += frame_size();

	if (avcodec_send_frame(ctx_, frame_) < 0)
		return false;
	while (avcodec_receive_packet(ctx_, packet_) == 0) {
		out.insert(out.end(), packet_->data, packet_->data + packet_->size);
		av_packet_unref(packet_);
	}
	return !out.empty();
}
