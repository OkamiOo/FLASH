#include "flash/sender.h"
#include "flash/audio_encoder.h"
#include "flash/clock.h"
#include "flash/log.h"
#include "flash/receiver.h"

#include <algorithm>
#include <cstdlib>

namespace flash {

namespace {
constexpr uint64_t kRepeatNs = 250'000'000ULL;   // image fixe : la dernière est réencodée 4 fois par seconde
constexpr uint64_t kIdrStaticNs = 30'000'000ULL; // image clé demandée sur une image fixe : réponse sous 30 ms
constexpr uint64_t kIdrMinNs = 250'000'000ULL;   // pas plus de 4 images clés par seconde à la demande
constexpr int64_t kAudioJumpNs = 5'000'000;      // au-delà, l'horodatage du son repart de celui du système

std::string json_escape(const std::string &s)
{
	std::string out;
	for (char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if ((unsigned char)c < 0x20) {
			out += ' ';
		} else {
			out += c;
		}
	}
	return out;
}
} // namespace

struct Sender::VideoTrack {
	uint16_t pid = 0;
	std::unique_ptr<VideoInput> input;
	std::unique_ptr<VideoEncoder> encoder;
	std::mutex mutex;
	ComPtr<ID3D11Texture2D> last; // copie de la dernière image, pour la répéter
	uint64_t last_encode = 0;
	uint64_t last_capture = 0;
	double tokens = 2.0;
	std::atomic<bool> want_idr{true};
	std::atomic<uint64_t> last_idr_request{0};
	EncodedFrame frame;
	TrackStats stats;
};

struct Sender::AudioTrack {
	uint16_t pid = 0;
	std::unique_ptr<AudioInput> input;
	AudioEncoder encoder;
	std::mutex mutex;
	std::vector<float> buf;
	uint32_t filled = 0;
	uint64_t buf_ts = 0;   // heure du premier échantillon du tampon
	uint64_t clock_ts = 0; // heure attendue du prochain échantillon reçu
	std::vector<uint8_t> packet;
	TrackStats stats;
};

Sender::Sender() : mux_([this](const uint8_t *data, size_t size) { server_.send(data, size); }) {}

Sender::~Sender()
{
	stop();
}

bool Sender::start(const SenderOptions &options, ComPtr<ID3D11Device> device,
		   std::vector<std::unique_ptr<VideoInput>> video, std::vector<std::unique_ptr<AudioInput>> audio)
{
	options_ = options;
	device_ = device;
	srt_init(true);

	std::vector<Track> tracks;
	for (size_t i = 0; i < video.size(); i++) {
		auto t = std::make_unique<VideoTrack>();
		t->pid = (uint16_t)(ts::kPidVideo0 + i);
		t->input = std::move(video[i]);
		t->stats.name = t->input->name();
		t->stats.video = true;

		VideoEncoderConfig vc;
		vc.width = t->input->width();
		vc.height = t->input->height();
		vc.fps_num = options.fps;
		vc.bitrate_kbps = options.video_kbps;
		vc.codec = options.codec;
		t->encoder = create_video_encoder(device.Get(), vc, options.encoder);
		if (!t->encoder) {
			FLOG_E("Aucun encodeur vidéo pour « %s »", t->stats.name.c_str());
			continue;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = vc.width;
		desc.Height = vc.height;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		device->CreateTexture2D(&desc, nullptr, &t->last);

		tracks.push_back({t->pid, options.codec});
		video_.push_back(std::move(t));
	}
	for (size_t i = 0; i < audio.size(); i++) {
		auto t = std::make_unique<AudioTrack>();
		t->pid = (uint16_t)(ts::kPidAudio0 + i);
		t->input = std::move(audio[i]);
		t->stats.name = t->input->name();
		if (!t->encoder.open(2, options.audio_bps))
			continue;
		t->buf.resize((size_t)t->encoder.frame_size() * 2);
		tracks.push_back({t->pid, Codec::Opus, 2});
		audio_.push_back(std::move(t));
	}
	if (video_.empty() && audio_.empty()) {
		FLOG_E("Aucune piste à envoyer");
		return false;
	}
	mux_.set_tracks(tracks);

	const bool listening = server_.start(
		options.srt,
		[this](int) {
			for (auto &t : video_)
				t->want_idr = true;
			mux_.write_tables();
		},
		[this](int client, const uint8_t *data, size_t size) { on_message(client, data, size); });
	if (!listening)
		return false;

	running_ = true;
	for (auto &t : video_) {
		VideoTrack *tp = t.get();
		if (!t->input->start(device.Get(), [this, tp](ID3D11Texture2D *nv12, uint64_t ts) { on_video(*tp, nv12, ts); }))
			FLOG_E("Impossible de démarrer « %s »", t->stats.name.c_str());
	}
	for (auto &t : audio_) {
		AudioTrack *tp = t.get();
		if (!t->input->start([this, tp](const float *s, uint32_t n, uint64_t ts) { on_audio(*tp, s, n, ts); }))
			FLOG_E("Impossible de démarrer « %s »", t->stats.name.c_str());
	}
	housekeeping_ = std::thread(&Sender::housekeeping, this);
	return true;
}

void Sender::stop()
{
	if (!running_.exchange(false))
		return;
	for (auto &t : video_)
		t->input->stop();
	for (auto &t : audio_)
		t->input->stop();
	if (housekeeping_.joinable())
		housekeeping_.join();
	server_.stop();
}

size_t Sender::client_count() const
{
	return server_.client_count();
}

// ---------- Vidéo ----------

void Sender::on_video(VideoTrack &t, ID3D11Texture2D *nv12, uint64_t capture_ns)
{
	// Cadence par seau à jetons : un jeton par intervalle, deux au plus. Une source à 60 i/s
	// passe en entier même quand Windows la date sur un autre rythme (écran à 165 Hz) ;
	// une source plus rapide est ramenée à la cadence voulue.
	{
		std::lock_guard lock(t.mutex);
		t.stats.captured++;
		if (t.last_capture && capture_ns > t.last_capture) {
			const double elapsed = (double)(capture_ns - t.last_capture) * options_.fps / 1e9;
			t.tokens = std::min(2.0, t.tokens + elapsed);
		}
		t.last_capture = capture_ns;
		if (t.tokens < 1.0)
			return;
		t.tokens -= 1.0;
	}
	encode_video(t, nv12, capture_ns);
}

void Sender::encode_video(VideoTrack &t, ID3D11Texture2D *nv12, uint64_t capture_ns, bool repeat)
{
	std::lock_guard lock(t.mutex);
	if (repeat) {
		// Revérifié sous verrou : une vraie image a pu arriver entre-temps
		const uint64_t idle = capture_ns - t.last_encode;
		if (!(idle > kRepeatNs || (t.want_idr && idle > kIdrStaticNs)))
			return;
	}
	if (nv12 != t.last.Get() && t.last) {
		ComPtr<ID3D11DeviceContext> ctx;
		device_->GetImmediateContext(&ctx);
		ctx->CopyResource(t.last.Get(), nv12);
	}

	const uint64_t start = now_ns();
	t.last_encode = start;
	if (!server_.wanted(t.pid))
		return;

	if (!t.encoder->encode(nv12, t.want_idr.exchange(false), t.frame))
		return;
	const uint64_t took = now_ns() - start;
	mux_.write_video(t.pid, capture_ns, t.frame.data.data(), t.frame.data.size(), t.frame.keyframe);

	t.stats.frames++;
	t.stats.keyframes += t.frame.keyframe;
	t.stats.bytes += t.frame.data.size();
	t.stats.encode_ms_avg += took / 1e6;
	t.stats.encode_ms_max = std::max(t.stats.encode_ms_max, took / 1e6);
}

// ---------- Son ----------

void Sender::on_audio(AudioTrack &t, const float *samples, uint32_t frames, uint64_t first_ns)
{
	std::lock_guard lock(t.mutex);

	// L'heure de chaque échantillon suit le compte des échantillons, recalé doucement sur
	// l'heure annoncée par le système : continu, et sans dériver de l'horloge commune.
	const int64_t err = (int64_t)(first_ns - t.clock_ts);
	if (!t.clock_ts || std::llabs(err) > kAudioJumpNs) {
		t.clock_ts = first_ns;
		t.filled = 0;
	} else {
		t.clock_ts += err / 32;
	}

	const uint32_t frame_size = (uint32_t)t.encoder.frame_size();
	const bool wanted = server_.wanted(t.pid);
	for (uint32_t i = 0; i < frames;) {
		if (t.filled == 0)
			t.buf_ts = t.clock_ts + (uint64_t)i * 1'000'000'000ULL / 48000;
		const uint32_t n = std::min(frames - i, frame_size - t.filled);
		std::copy(samples + (size_t)i * 2, samples + (size_t)(i + n) * 2, t.buf.begin() + (size_t)t.filled * 2);
		t.filled += n;
		i += n;

		if (t.filled == frame_size) {
			t.filled = 0;
			if (!wanted)
				continue;
			const uint64_t start = now_ns();
			if (!t.encoder.encode(t.buf.data(), t.packet))
				continue;
			// Le PTS désigne le premier échantillon décodé : l'encodeur décale de son retard
			const uint64_t pts = t.buf_ts - (uint64_t)t.encoder.lookahead() * 1'000'000'000ULL / 48000;
			mux_.write_audio(t.pid, pts, t.packet.data(), t.packet.size());
			const double took = (now_ns() - start) / 1e6;
			t.stats.frames++;
			t.stats.bytes += t.packet.size();
			t.stats.encode_ms_avg += took;
			t.stats.encode_ms_max = std::max(t.stats.encode_ms_max, took);
		}
	}
	t.clock_ts += (uint64_t)frames * 1'000'000'000ULL / 48000;
}

// ---------- Voie de retour, horloge, tables ----------

void Sender::on_message(int client, const uint8_t *data, size_t size)
{
	if (size >= 3 && data[0] == control::kKeyframeRequest) {
		const uint16_t pid = (uint16_t)((data[1] << 8) | data[2]);
		const uint64_t now = now_ns();
		for (auto &t : video_) {
			if (t->pid == pid && now - t->last_idr_request > kIdrMinNs) {
				t->last_idr_request = now;
				t->want_idr = true;
			}
		}
	} else if (size >= 1 && data[0] == control::kSubscribe) {
		std::vector<uint16_t> pids;
		for (size_t i = 1; i + 1 < size; i += 2)
			pids.push_back((uint16_t)((data[i] << 8) | data[i + 1]));
		server_.set_subscription(client, pids);
		// Une piste qu'on se met à envoyer commence par une image clé
		for (auto &t : video_)
			t->want_idr = true;
	}
}

void Sender::housekeeping()
{
	const std::string meta = metadata();
	uint64_t tick = now_ns();
	for (uint64_t n = 0; running_; n++) {
		tick += 10'000'000ULL;
		sleep_until_ns(tick);
		const uint64_t now = now_ns();

		if (n % 2 == 0)
			mux_.write_pcr(now);
		if (n % 10 == 0)
			mux_.write_tables();
		if (n % 100 == 0)
			mux_.write_meta(meta);

		// Image fixe (bureau immobile, fenêtre cachée) : on répète la dernière image,
		// tout de suite si un récepteur attend une image clé.
		for (auto &t : video_) {
			uint64_t last;
			{
				std::lock_guard lock(t->mutex);
				last = t->last_encode;
			}
			const uint64_t idle = now - last;
			if (last && t->last && (idle > kRepeatNs || (t->want_idr && idle > kIdrStaticNs)))
				encode_video(*t, t->last.Get(), now, true);
		}
	}
}

std::string Sender::tracks_json() const
{
	std::string m = "[";
	bool first = true;
	for (const auto &t : video_) {
		m += (first ? "" : ",") + std::string("{\"pid\":") + std::to_string(t->pid) +
		     ",\"type\":\"video\",\"nom\":\"" + json_escape(t->stats.name) + "\",\"largeur\":" +
		     std::to_string(t->input->width()) + ",\"hauteur\":" + std::to_string(t->input->height()) + "}";
		first = false;
	}
	for (const auto &t : audio_) {
		m += (first ? "" : ",") + std::string("{\"pid\":") + std::to_string(t->pid) +
		     ",\"type\":\"audio\",\"nom\":\"" + json_escape(t->stats.name) + "\"}";
		first = false;
	}
	return m + "]";
}

std::string Sender::metadata() const
{
	return "{\"v\":1,\"nom\":\"" + json_escape(options_.host_name) + "\",\"pistes\":" + tracks_json() + "}";
}

std::vector<TrackStats> Sender::stats()
{
	std::vector<TrackStats> out;
	for (auto &t : video_) {
		std::lock_guard lock(t->mutex);
		TrackStats s = t->stats;
		s.encode_ms_avg = s.frames ? s.encode_ms_avg / s.frames : 0;
		s.active = server_.wanted(t->pid);
		out.push_back(s);
		t->stats = TrackStats{t->stats.name, true};
	}
	for (auto &t : audio_) {
		std::lock_guard lock(t->mutex);
		TrackStats s = t->stats;
		s.encode_ms_avg = s.frames ? s.encode_ms_avg / s.frames : 0;
		s.active = server_.wanted(t->pid);
		out.push_back(s);
		t->stats = TrackStats{t->stats.name, false};
	}
	return out;
}

} // namespace flash
