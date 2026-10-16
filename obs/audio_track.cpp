#include "audio_track.h"
#include "connection.h"

#include <flash/clock.h>
#include <flash/log.h>
#include <flash/measure.h>
#include <flash/ts.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>

namespace {
constexpr int64_t kDeadbandNs = 250'000;     // en deçà, on ne corrige pas
constexpr int64_t kFastNs = 20'000'000;      // au-delà, correction plus rapide (1 %)
constexpr int64_t kResetNs = 100'000'000;    // au-delà, on repart de l'heure voulue
} // namespace

AudioTrack::AudioTrack(Connection &conn, uint16_t pid) : conn_(conn), pid_(pid)
{
	if (!open())
		FLOG_E("Décodeur Opus indisponible");
	thread_ = std::thread(&AudioTrack::run, this);
}

AudioTrack::~AudioTrack()
{
	stop_ = true;
	{
		std::lock_guard lock(queue_mutex_);
	}
	queue_cv_.notify_all();
	thread_.join();

	swr_free(&swr_);
	av_packet_free(&packet_);
	av_frame_free(&frame_);
	avcodec_free_context(&ctx_);
}

bool AudioTrack::open()
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec)
		return false;
	ctx_ = avcodec_alloc_context3(codec);
	ctx_->sample_rate = 48000;
	av_channel_layout_default(&ctx_->ch_layout, 2);
	if (avcodec_open2(ctx_, codec, nullptr) < 0)
		return false;

	out_rate_ = audio_output_get_sample_rate(obs_get_audio());
	AVChannelLayout stereo;
	av_channel_layout_default(&stereo, 2);
	if (swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLTP, (int)out_rate_, &stereo, AV_SAMPLE_FMT_FLTP, 48000, 0,
				nullptr) < 0 ||
	    swr_init(swr_) < 0)
		return false;

	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	return true;
}

void AudioTrack::set_sources(std::vector<obs_source_t *> sources)
{
	std::lock_guard lock(mutex_);
	sources_ = std::move(sources);
}

void AudioTrack::push(const flash::MediaPacket &pkt)
{
	{
		std::lock_guard lock(queue_mutex_);
		if (queue_.size() > 50) // une demi-seconde de retard : on repart à neuf
			queue_.clear();
		queue_.push_back({pkt.pts_ns, {pkt.data.begin(), pkt.data.end()}});
	}
	queue_cv_.notify_one();
}

void AudioTrack::run()
{
	for (;;) {
		Packet pkt;
		{
			std::unique_lock lock(queue_mutex_);
			queue_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
			if (stop_)
				break;
			pkt = std::move(queue_.front());
			queue_.pop_front();
		}
		if (ctx_ && swr_)
			decode(pkt);
	}
}

void AudioTrack::decode(const Packet &pkt)
{
	std::vector<std::span<const uint8_t>> packets;
	if (!flash::split_opus_pes(pkt.data.data(), pkt.data.size(), packets))
		return;

	uint64_t pts = pkt.pts_ns;
	for (const auto &p : packets) {
		packet_->data = (uint8_t *)p.data();
		packet_->size = (int)p.size();
		if (avcodec_send_packet(ctx_, packet_) < 0)
			continue;
		while (avcodec_receive_frame(ctx_, frame_) == 0) {
			flash::ClockMap &clock = conn_.clock();
			if (clock.ready()) {
				// Le son vise l'instant où OBS affichera l'image de même heure
				const uint64_t intended = clock.present(pts) + Connection::video_interval_ns() / 2 +
							  (uint64_t)conn_.phase_ns.load();
				output(frame_, intended);
			}
			pts += (uint64_t)frame_->nb_samples * 1'000'000'000ULL / 48000;
			av_frame_unref(frame_);
		}
	}
}

void AudioTrack::output(const AVFrame *frame, uint64_t intended)
{
	std::lock_guard lock(mutex_);
	if (sources_.empty())
		return;

	const int64_t error = timeline_ ? (int64_t)(next_ts_ - intended) : 0;
	if (!timeline_ || std::llabs(error) > kResetNs) {
		next_ts_ = intended;
		timeline_ = true;
		swr_set_compensation(swr_, 0, 0);
	} else if (std::llabs(error) > kDeadbandNs) {
		// Positif : OBS jouerait trop tard, on retire des échantillons (et inversement)
		const int64_t limit = (int64_t)out_rate_ / (std::llabs(error) > kFastNs ? 100 : 500);
		int64_t delta = -error * (int64_t)out_rate_ / 1'000'000'000LL;
		delta = std::clamp(delta, -limit, limit);
		swr_set_compensation(swr_, (int)delta, (int)out_rate_);
	} else {
		swr_set_compensation(swr_, 0, 0);
	}

	const int max_out = swr_get_out_samples(swr_, frame->nb_samples);
	if (max_out <= 0)
		return;
	out_[0].resize((size_t)max_out);
	out_[1].resize((size_t)max_out);
	uint8_t *planes[2] = {(uint8_t *)out_[0].data(), (uint8_t *)out_[1].data()};
	const int n = swr_convert(swr_, planes, max_out, (const uint8_t **)frame->extended_data, frame->nb_samples);
	if (n <= 0)
		return;

	struct obs_source_audio a = {};
	a.data[0] = planes[0];
	a.data[1] = planes[1];
	a.frames = (uint32_t)n;
	a.speakers = SPEAKERS_STEREO;
	a.format = AUDIO_FORMAT_FLOAT_PLANAR;
	a.samples_per_sec = out_rate_;
	a.timestamp = next_ts_;
	for (obs_source_t *s : sources_)
		obs_source_output_audio(s, &a);

	if (flash::measure::enabled())
		detect_beep(out_[0].data(), (uint32_t)n, next_ts_);

	// Même calcul qu'OBS pour enchaîner les blocs
	next_ts_ += (uint64_t)n * 1'000'000'000ULL / out_rate_;
}

void AudioTrack::detect_beep(const float *left, uint32_t frames, uint64_t first_ns)
{
	for (uint32_t i = 0; i < frames; i++) {
		if (std::fabs(left[i]) > 0.05f) {
			if (!in_beep_ && quiet_samples_ > out_rate_ / 10)
				flash::measure::beep(first_ns + (uint64_t)i * 1'000'000'000ULL / out_rate_);
			in_beep_ = true;
			quiet_samples_ = 0;
		} else if (++quiet_samples_ > out_rate_ / 100) {
			in_beep_ = false;
		}
	}
}
