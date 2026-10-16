#include "video_track.h"
#include "connection.h"
#include "measure.h"

#include <flash/clock.h>
#include <flash/log.h>
#include <flash/mire.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace {

constexpr size_t kMaxQueue = 30;          // au-delà d'une demi-seconde de retard, on repart d'une image clé
constexpr size_t kMaxReady = 6;           // images décodées en attente de remise
constexpr uint64_t kGuardNs = 1'500'000;  // marge avec l'instant où OBS choisit l'image à afficher
constexpr uint64_t kWarmupFrames = 30;

AVPixelFormat pick_hw_format(AVCodecContext *, const AVPixelFormat *formats)
{
	for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_D3D11)
			return *p;
	}
	return formats[0];
}

} // namespace

VideoTrack::VideoTrack(Connection &conn, uint16_t pid, flash::Codec codec) : conn_(conn), pid_(pid), codec_(codec)
{
	decode_thread_ = std::thread(&VideoTrack::decode_loop, this);
	release_thread_ = std::thread(&VideoTrack::release_loop, this);
}

VideoTrack::~VideoTrack()
{
	stop_ = true;
	{
		std::lock_guard lock(queue_mutex_);
	}
	queue_cv_.notify_all();
	{
		std::lock_guard lock(ready_mutex_);
	}
	ready_cv_.notify_all();
	decode_thread_.join();
	release_thread_.join();

	for (Ready &r : ready_)
		av_frame_free(&r.frame);
	for (AVFrame *f : pool_)
		av_frame_free(&f);
}

void VideoTrack::push(const flash::MediaPacket &pkt)
{
	Packet p{pkt.pts_ns, pkt.arrival_ns, {pkt.data.begin(), pkt.data.end()}, pkt.keyframe, pkt.corrupt};
	{
		std::lock_guard lock(queue_mutex_);
		if (queue_.size() >= kMaxQueue) {
			// Le décodage n'a pas suivi : on jette tout et on repart d'une image clé
			queue_.clear();
			p.corrupt = true;
		}
		queue_.push_back(std::move(p));
	}
	queue_cv_.notify_one();
}

void VideoTrack::set_sources(std::vector<obs_source_t *> sources)
{
	std::lock_guard lock(sources_mutex_);
	sources_ = std::move(sources);
}

// ---------- Décodage ----------

void VideoTrack::decode_loop()
{
	const bool ok = open_decoder();
	conn_.request_keyframe(pid_);

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
		if (ok)
			decode(pkt);
	}

	close_decoder();
}

bool VideoTrack::open_decoder()
{
	const AVCodec *codec = avcodec_find_decoder(codec_ == flash::Codec::HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	if (!codec)
		return false;

	ctx_ = avcodec_alloc_context3(codec);
	if (av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0) {
		ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
		ctx_->get_format = pick_hw_format;
	} else {
		FLOG_W("Décodage matériel indisponible, décodage par le processeur");
	}
	ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx_->thread_count = 1; // le multi-fil par images ajoute une image de retard

	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		FLOG_E("Ouverture du décodeur vidéo impossible");
		return false;
	}
	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	return true;
}

void VideoTrack::close_decoder()
{
	av_packet_free(&packet_);
	av_frame_free(&frame_);
	avcodec_free_context(&ctx_);
	av_buffer_unref(&hw_device_);
}

void VideoTrack::decode(const Packet &pkt)
{
	if (pkt.corrupt)
		conn_.request_keyframe(pid_);

	if (waiting_keyframe_) {
		if (!pkt.keyframe) {
			conn_.request_keyframe(pid_);
			return;
		}
		waiting_keyframe_ = false;
	}

	packet_->data = (uint8_t *)pkt.data.data();
	packet_->size = (int)pkt.data.size();
	packet_->pts = (int64_t)pkt.pts_ns;
	if (avcodec_send_packet(ctx_, packet_) < 0) {
		conn_.request_keyframe(pid_);
		return;
	}

	while (avcodec_receive_frame(ctx_, frame_) == 0) {
		AVFrame *out = take_frame();
		bool ok;
		if (frame_->format == AV_PIX_FMT_D3D11) {
			if (out->buf[0] && (out->width != frame_->width || out->height != frame_->height))
				av_frame_unref(out);
			ok = av_hwframe_transfer_data(out, frame_, 0) == 0;
		} else {
			av_frame_unref(out);
			ok = av_frame_ref(out, frame_) == 0;
		}
		av_frame_unref(frame_);

		if (ok)
			schedule(out, pkt, flash::now_ns());
		else
			give_frame(out);
	}
}

// Fixe l'heure de remise : celle de l'horloge commune, décalée pour ne jamais tomber pile
// au moment où OBS choisit l'image de son prochain rendu.
void VideoTrack::schedule(AVFrame *frame, const Packet &pkt, uint64_t decoded_ns)
{
	flash::ClockMap &clock = conn_.clock();
	const uint64_t pts = cadence_.smooth(pkt.pts_ns);
	// Les premières images comptent l'initialisation du décodeur : on ne les fait pas
	// entrer dans l'horloge, elles gonfleraient la latence pour longtemps.
	if (decoded_++ >= kWarmupFrames)
		clock.observe(pts, decoded_ns);
	if (!clock.ready()) {
		give_frame(frame);
		return;
	}

	const uint64_t interval = Connection::video_interval_ns();
	const uint64_t target = clock.present(pts);
	const uint64_t now = flash::now_ns();
	const uint64_t tick = obs_get_video_frame_time();

	// Le premier rendu d'OBS à partir de l'heure visée : l'horloge garantit que toutes les
	// images sont prêtes à cette heure-là, donc chacune a son rendu, sans collision. (Viser
	// un rendu plus tôt fait perdre les images que le GPU livre en retard quand OBS encode.)
	// L'image est remise au milieu de l'intervalle qui précède ce rendu : ni trop tôt
	// (elle passerait au rendu d'avant), ni trop près du rendu lui-même.
	uint64_t display = target > tick ? tick + (target - tick + interval - 1) / interval * interval : tick;

	// Heure visée à la frontière entre deux rendus : on garde le côté choisi pour l'image
	// précédente tant qu'on reste à moins de 1,5 ms de la frontière, sinon la moindre gigue
	// fait alterner les images entre deux rendus. Au-delà, on bascule une seule fois.
	const int64_t d = (int64_t)(display - target);
	if (have_prev_) {
		const int64_t span = (int64_t)interval;
		int64_t best = d;
		for (const int64_t cand : {d - span, d + span}) {
			if (cand >= -(int64_t)kGuardNs && cand < span + (int64_t)kGuardNs &&
			    std::llabs(cand - prev_d_) < std::llabs(best - prev_d_))
				best = cand;
		}
		display = (uint64_t)((int64_t)target + best);
	}
	while (display < now + kGuardNs)
		display += interval;
	prev_d_ = (int64_t)(display - target);
	have_prev_ = true;
	uint64_t release = display - interval / 2;
	if (release < now)
		release = now;

	// Phase moyenne entre l'affichage réel et le milieu de l'intervalle visé : le son s'y aligne
	if (conn_.phase_pid == pid_) {
		const int64_t sample = (int64_t)(display - target) - (int64_t)interval / 2;
		const int64_t phase = conn_.phase_ns.load();
		conn_.phase_ns = frames_++ == 0 ? sample : phase + (sample - phase) / 32;
	}

	{
		std::lock_guard lock(ready_mutex_);
		while (ready_.size() >= kMaxReady) {
			pool_.push_back(ready_.front().frame); // trop d'attente : on jette la plus ancienne
			ready_.pop_front();
		}
		ready_.push_back({frame, pkt.pts_ns, pkt.arrival_ns, decoded_ns, target, release});
	}
	ready_cv_.notify_one();
}

AVFrame *VideoTrack::take_frame()
{
	std::lock_guard lock(ready_mutex_);
	if (pool_.empty())
		return av_frame_alloc();
	AVFrame *f = pool_.back();
	pool_.pop_back();
	return f;
}

// Le tampon de l'image reste alloué pour la suivante
void VideoTrack::give_frame(AVFrame *frame)
{
	std::lock_guard lock(ready_mutex_);
	pool_.push_back(frame);
}

// ---------- Remise ----------

void VideoTrack::release_loop()
{
	for (;;) {
		Ready r;
		{
			std::unique_lock lock(ready_mutex_);
			ready_cv_.wait(lock, [this] { return stop_ || !ready_.empty(); });
			if (stop_)
				break;
			r = ready_.front();
			ready_.pop_front();
		}

		flash::sleep_until_ns(r.release_ns);
		output(r);
		give_frame(r.frame);
	}
}

void VideoTrack::output(const Ready &r)
{
	AVFrame *frame = r.frame;
	struct obs_source_frame f = {};
	if (frame->format == AV_PIX_FMT_NV12)
		f.format = VIDEO_FORMAT_NV12;
	else if (frame->format == AV_PIX_FMT_YUV420P)
		f.format = VIDEO_FORMAT_I420;
	else
		return;

	f.width = frame->width;
	f.height = frame->height;
	f.timestamp = r.target_ns;
	for (int i = 0; i < 3; i++) {
		f.data[i] = frame->data[i];
		f.linesize[i] = (uint32_t)frame->linesize[i];
	}
	video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, f.format, f.color_matrix,
					       f.color_range_min, f.color_range_max);

	const uint64_t released = flash::now_ns();
	{
		std::lock_guard lock(sources_mutex_);
		for (obs_source_t *s : sources_) {
			obs_source_output_video(s, &f);
			measure::released(s, r.target_ns);
		}
	}

	// Mesure : la première piste vidéo seulement (celle de la mire ou du banc)
	if (measure::enabled() && pid_ == flash::ts::kPidVideo0) {
		uint64_t capture = 0;
		uint32_t n = 0;
		if (flash::mire::read_code(frame->data[0], frame->linesize[0], capture, n))
			measure::frame(n, capture, r.arrival_ns, r.decoded_ns, released, r.target_ns,
				       flash::mire::flash_visible(frame->data[0], frame->linesize[0]), r.pts_ns);
	}
}
