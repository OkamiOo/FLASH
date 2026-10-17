// Visionneuse de l'appli : une piste vidéo et des pistes son d'un émetteur, dans une fenêtre.
//
// L'image est décodée par Windows (Media Foundation) puis copiée dans une petite réserve de
// textures. Comme dans OBS, chaque image a une heure de présentation donnée par l'horloge
// commune : elle est présentée au milieu de l'intervalle qui précède le premier balayage de
// l'écran à partir de cette heure, avec la même hystérésis. Le son de toutes les pistes est
// rangé sur l'horloge de l'émetteur, puis lu par la sortie choisie au moment où l'image de
// même heure est à l'écran.

#include "viewer.h"
#include "resource.h"

#include <flash/audio_decoder.h>
#include <flash/audio_output.h>
#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/measure.h>
#include <flash/mire.h>
#include <flash/receiver.h>
#include <flash/video.h>

#include <windows.h>
#include <d2d1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using json = nlohmann::json;
using namespace flash;

namespace {

constexpr uint32_t kRate = 48000;
constexpr int kSlots = 10;
constexpr size_t kMaxQueue = 30;          // au-delà d'une demi-seconde de retard, on repart d'une image clé
constexpr size_t kMaxReady = 8;           // images décodées en attente de présentation
constexpr uint64_t kGuardNs = 1'500'000;  // marge avec le balayage de l'écran
constexpr uint64_t kWarmupFrames = 30;
constexpr uint64_t kStallNs = 1'000'000'000ULL; // décodeur sans image pendant ce temps : refait
constexpr UINT WM_VIEWER_DESTROY = WM_APP + 10;
constexpr wchar_t kClass[] = L"FlashVisionneuse";

// Le son de toutes les pistes, rangé par heure de l'émetteur : la case d'un échantillon est
// son index (48 000 par seconde) modulo la taille de l'anneau. Chaque case garde l'index qu'elle
// porte : une case d'un autre tour compte comme du silence, il n'y a rien à effacer.
class Mixer {
public:
	struct Writer {
		int64_t next = 0;
		bool started = false;
	};

	// Écrit à la suite de ce que la piste a déjà écrit. L'heure annoncée peut s'en écarter un
	// peu (dérive entre horloges) : ce bloc est alors étiré ou resserré d'un échantillon.
	void write(Writer &w, uint64_t pts_ns, const float *pcm, int frames)
	{
		if (frames <= 0)
			return;
		const int64_t at = (int64_t)(pts_ns * 6 / 125'000); // × 48 000 / 1e9
		if (!w.started || std::llabs(at - w.next) > (int64_t)kRate / 200) {
			w.next = at;
			w.started = true;
		}
		int out = frames;
		if (at - w.next > 2)
			out++;
		else if (w.next - at > 2)
			out--;

		std::lock_guard lock(mutex_);
		for (int j = 0; j < out; j++) {
			const double x = out > 1 ? (double)j * (frames - 1) / (out - 1) : 0.0;
			const int i = (int)x;
			const int i2 = std::min(i + 1, frames - 1);
			const float f = (float)(x - i);
			const float l = pcm[i * 2] + (pcm[i2 * 2] - pcm[i * 2]) * f;
			const float r = pcm[i * 2 + 1] + (pcm[i2 * 2 + 1] - pcm[i * 2 + 1]) * f;
			const int64_t k = w.next + j;
			Slot &s = ring_[(size_t)(k & kMask)];
			if (s.index != k) {
				s.index = k;
				s.l = l;
				s.r = r;
			} else {
				s.l += l;
				s.r += r;
			}
		}
		w.next += out;
	}

	// Lit frames échantillons stéréo à partir de pos (en échantillons, fractionnaire), au pas donné
	void read(double &pos, double step, float *out, uint32_t frames)
	{
		std::lock_guard lock(mutex_);
		for (uint32_t j = 0; j < frames; j++) {
			const double fl = std::floor(pos);
			const int64_t i = (int64_t)fl;
			const float f = (float)(pos - fl);
			const Slot &a = ring_[(size_t)(i & kMask)];
			const Slot &b = ring_[(size_t)((i + 1) & kMask)];
			const float al = a.index == i ? a.l : 0.0f, ar = a.index == i ? a.r : 0.0f;
			const float bl = b.index == i + 1 ? b.l : 0.0f, br = b.index == i + 1 ? b.r : 0.0f;
			out[j * 2] = al + (bl - al) * f;
			out[j * 2 + 1] = ar + (br - ar) * f;
			pos += step;
		}
	}

private:
	struct Slot {
		int64_t index = -1;
		float l = 0, r = 0;
	};
	static constexpr int64_t kSize = 1 << 16; // 1,4 s
	static constexpr int64_t kMask = kSize - 1;

	std::mutex mutex_;
	std::vector<Slot> ring_ = std::vector<Slot>((size_t)kSize);
};

RECT fit_rect(const RECT &src, uint32_t w, uint32_t h)
{
	const double sw = std::max<LONG>(1, src.right - src.left), sh = std::max<LONG>(1, src.bottom - src.top);
	const double scale = std::min(w / sw, h / sh);
	const LONG dw = (LONG)std::lround(sw * scale), dh = (LONG)std::lround(sh * scale);
	const LONG x = ((LONG)w - dw) / 2, y = ((LONG)h - dh) / 2;
	return {x, y, x + dw, y + dh};
}

} // namespace

struct Viewer::Impl {
	explicit Impl(const ViewerOptions &options);
	~Impl();

	json status();

	// Réception
	void on_streams(const std::vector<TsStream> &streams);
	void on_packet(const MediaPacket &pkt);
	void on_state(bool connected);
	void on_meta(std::span<const uint8_t> data);
	void rebind();
	void request_keyframe();

	// Son
	void on_audio(const MediaPacket &pkt);
	void fill(float *out, uint32_t frames, uint64_t first_ns);
	void detect_beep(const float *samples, uint32_t frames, uint64_t first_ns);

	// Décodage
	struct Info {
		int64_t time;
		uint64_t pts, arrival;
	};
	void decode_loop();
	void on_frame(const DecodedFrame &f, std::deque<Info> &infos);
	int take_slot(uint32_t w, uint32_t h);
	void free_slot(int s);

	// Fenêtre et affichage
	static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	void window_loop();
	void set_fullscreen(HWND h, bool on);
	void vblank_loop();
	void present_loop();
	bool create_swap_chain();
	void resize();
	bool ensure_processor(uint32_t in_w, uint32_t in_h);
	struct Ready;
	void plan(Ready &f);
	bool draw(const Ready &f);
	void draw_text(const std::wstring &text);
	void frame_shown(const Ready &f, uint64_t shown_ns);
	std::wstring status_text();

	ViewerOptions o;

	// Réception
	Receiver receiver_;
	ClockMap clock_;
	std::mutex streams_mutex_;
	std::vector<TsStream> streams_;
	std::map<uint16_t, std::string> names_;
	std::string meta_;
	std::atomic<uint16_t> video_pid_{0};
	std::atomic<bool> names_known_{false};
	std::atomic<uint64_t> last_keyframe_request_{0};
	std::atomic<bool> local_{false};
	uint64_t last_report_ = 0;
	int64_t max_video_lag_ = INT64_MIN, max_audio_lag_ = INT64_MIN;

	// Son
	struct AudioTrack {
		AudioDecoder decoder;
		Mixer::Writer writer;
	};
	std::mutex audio_mutex_;
	std::map<uint16_t, std::unique_ptr<AudioTrack>> audio_;
	std::vector<float> pcm_ = std::vector<float>(5760 * 2);
	Mixer mixer_;
	AudioOutput out_;
	bool out_ok_ = false;
	double cursor_ = 0; // lecture, en échantillons de l'heure de l'émetteur
	bool have_cursor_ = false;
	bool in_beep_ = false;
	uint32_t quiet_samples_ = 0;

	// Décodage
	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> ctx_;
	struct Packet {
		uint16_t pid;
		Codec codec;
		uint64_t pts, arrival;
		std::vector<uint8_t> data;
		bool keyframe, corrupt;
	};
	std::thread decode_thread_;
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<Packet> queue_;
	CadenceSmoother cadence_;
	uint64_t decoded_ = 0;
	uint64_t matched_ = 0; // images sorties du décodeur et reconnues (fil de décodage)
	ComPtr<ID3D11Texture2D> probe_; // mesure : copie lisible de l'image
	std::mutex info_mutex_;
	std::string decoder_name_;
	std::string error_;
	std::string note_; // format choisi par l'émetteur que ce PC ne décode pas
	uint8_t formats_ = 0, installed_ = 0;

	// Images prêtes
	struct Slot {
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<ID3D11VideoProcessorInputView> view;
		uint32_t w = 0, h = 0;
		uint32_t view_gen = 0;
		bool busy = false;
	};
	struct Ready {
		int slot = -1;
		RECT visible{};
		uint64_t pts = 0, target = 0, arrival = 0, decoded = 0;
		uint64_t show = 0, release = 0;
		bool planned = false;
		bool code = false; // mire lue
		uint64_t capture = 0;
		uint32_t number = 0;
		bool flash = false;
	};
	std::mutex ready_mutex_;
	std::condition_variable ready_cv_;
	std::array<Slot, kSlots> slots_;
	std::deque<Ready> ready_;

	// Fenêtre
	std::thread window_thread_;
	std::atomic<HWND> hwnd_{nullptr};
	std::atomic<bool> stop_{false}, closing_{false}, closed_{false}, resized_{false};
	bool fullscreen_ = false;
	WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
	ULONGLONG last_mouse_ = 0;

	// Balayage de l'écran
	std::mutex grid_mutex_;
	uint64_t grid_ = 0, period_ = 0, last_vb_ = 0;
	std::vector<uint64_t> warmup_;

	// Affichage (fil de présentation seulement)
	ComPtr<IDXGISwapChain1> swap_;
	uint32_t sw_w_ = 0, sw_h_ = 0;
	ComPtr<ID3D11VideoDevice> vdev_;
	ComPtr<ID3D11VideoContext1> vctx_;
	ComPtr<ID3D11VideoProcessorEnumerator> venum_;
	ComPtr<ID3D11VideoProcessor> vproc_;
	uint32_t proc_in_w_ = 0, proc_in_h_ = 0, proc_out_w_ = 0, proc_out_h_ = 0, proc_gen_ = 0;
	std::unordered_map<ID3D11Texture2D *, ComPtr<ID3D11VideoProcessorOutputView>> out_views_;
	ComPtr<ID2D1Factory> d2d_;
	ComPtr<IDWriteFactory> dwrite_;
	ComPtr<IDWriteTextFormat> text_format_;
	std::wstring last_text_;
	uint64_t last_present_ = 0;
	int64_t prev_d_ = 0;
	bool have_prev_ = false;
	struct Pending {
		uint64_t target, show, pts;
	};
	std::map<UINT, Pending> pending_;
	bool stats_ok_ = false;
	int64_t correction_ = 0; // heure réelle d'affichage - balayage prévu
	uint32_t failures_ = 0;

	// État
	std::atomic<int64_t> phase_{0};
	std::atomic<bool> have_phase_{false};
	std::atomic<uint64_t> shown_{0}, dropped_{0};
	std::atomic<uint64_t> latency_sum_{0}, latency_n_{0};
	std::atomic<uint64_t> display_sum_{0}, display_count_{0}; // de l'arrivée à l'affichage, pour l'émetteur
	uint64_t stats_at_ = 0;
};

// ---------- Mise en route ----------

Viewer::Impl::Impl(const ViewerOptions &options) : o(options)
{
	device_ = create_d3d11_device();
	if (device_)
		device_->GetImmediateContext(&ctx_);
	else
		error_ = "Aucune carte graphique utilisable";

	if (!o.audio.empty()) {
		out_ok_ = out_.start(o.output, [this](float *samples, uint32_t frames, uint64_t first_ns) {
			fill(samples, frames, first_ns);
		});
	}

	window_thread_ = std::thread(&Impl::window_loop, this);
	decode_thread_ = std::thread(&Impl::decode_loop, this);

	SrtOptions srt = o.srt;
	auto refresh = srt.refresh;
	srt.refresh = [this, refresh](SrtOptions &opt) {
		const bool ok = !refresh || refresh(opt);
		local_ = opt.address == "127.0.0.1";
		return ok;
	};
	local_ = srt.address == "127.0.0.1";
	// L'émetteur n'envoie que des formats que Windows sait décoder ici
	formats_ = device_ ? mf_decoder_formats(device_.Get(), &installed_) : formats::kH264;
	receiver_.set_formats(formats_);
	receiver_.start(
		srt, [this](const std::vector<TsStream> &s) { on_streams(s); }, [this](const MediaPacket &p) { on_packet(p); },
		[this](bool connected) { on_state(connected); });
}

Viewer::Impl::~Impl()
{
	receiver_.stop();
	out_.stop();
	stop_ = true;
	{
		std::lock_guard lock(queue_mutex_);
	}
	queue_cv_.notify_all();
	decode_thread_.join();
	{
		std::lock_guard lock(ready_mutex_);
	}
	ready_cv_.notify_all();
	window_thread_.join();
}

// ---------- Réception ----------

void Viewer::Impl::request_keyframe()
{
	const uint16_t pid = video_pid_;
	const uint64_t now = now_ns();
	uint64_t prev = last_keyframe_request_.load();
	if (!pid || now - prev < 250'000'000ULL || !last_keyframe_request_.compare_exchange_strong(prev, now))
		return;
	receiver_.request_keyframe(pid);
}

// Pistes voulues d'après leurs noms ; l'émetteur n'envoie que celles-là
void Viewer::Impl::rebind()
{
	uint16_t video = 0;
	std::vector<uint16_t> sounds;
	for (const TsStream &st : streams_) {
		auto it = names_.find(st.pid);
		const std::string name = it != names_.end() ? it->second : "";
		if (is_video(st.codec)) {
			if (!video && o.video != "-" &&
			    (o.video.empty() || (!name.empty() && _stricmp(name.c_str(), o.video.c_str()) == 0)))
				video = st.pid;
		} else if (st.codec == Codec::Opus) {
			for (const std::string &a : o.audio) {
				if (a == "*" || (!name.empty() && _stricmp(name.c_str(), a.c_str()) == 0)) {
					sounds.push_back(st.pid);
					break;
				}
			}
		}
	}

	const uint16_t previous = video_pid_.exchange(video);
	{
		std::lock_guard lock(audio_mutex_);
		for (auto it = audio_.begin(); it != audio_.end();)
			it = std::find(sounds.begin(), sounds.end(), it->first) != sounds.end() ? std::next(it) : audio_.erase(it);
		for (uint16_t pid : sounds) {
			auto &t = audio_[pid];
			if (!t) {
				t = std::make_unique<AudioTrack>();
				t->decoder.open();
			}
		}
	}

	std::vector<uint16_t> pids = sounds;
	if (video)
		pids.insert(pids.begin(), video);
	if (pids.empty())
		pids.push_back(0x1FFF);
	receiver_.subscribe(std::move(pids));
	if (video && video != previous)
		request_keyframe();
}

void Viewer::Impl::on_meta(std::span<const uint8_t> data)
{
	const std::string text(data.begin(), data.end());
	if (text == meta_)
		return;
	meta_ = text;
	const json j = json::parse(text, nullptr, false);
	if (!j.is_object() || !j.contains("pistes") || !j["pistes"].is_array())
		return;
	names_.clear();
	for (const auto &t : j["pistes"]) {
		if (t.is_object())
			names_[(uint16_t)t.value("pid", 0)] = t.value("nom", "");
	}
	names_known_ = true;

	// Format choisi par l'émetteur que ce PC ne décode pas : l'image arrive en H.264, on dit pourquoi
	const std::string chosen = j.value("format_choisi", "");
	std::string note;
	if (chosen == "AV1" && !(formats_ & formats::kAV1))
		note = installed_ & formats::kAV1
			       ? "L'émetteur a choisi l'AV1, que la carte graphique de ce PC ne décode pas : l'image arrive en H.264"
			       : "L'émetteur a choisi l'AV1 : pour le recevoir, installez l'extension AV1 de Windows "
				 "(« AV1 Video Extension », Microsoft Store). En attendant, l'image arrive en H.264";
	else if (chosen == "HEVC" && !(formats_ & formats::kHEVC))
		note = "L'émetteur a choisi le HEVC : pour le recevoir, installez l'extension HEVC de Windows. En "
		       "attendant, l'image arrive en H.264";
	{
		std::lock_guard lock(info_mutex_);
		if (note != note_ && !note.empty())
			FLOG_W("%s", note.c_str());
		note_ = note;
	}
	rebind();
}

void Viewer::Impl::on_streams(const std::vector<TsStream> &streams)
{
	std::lock_guard lock(streams_mutex_);
	streams_ = streams;
	clock_.relax();
	rebind();
}

void Viewer::Impl::on_state(bool connected)
{
	if (connected) {
		FLOG_I("Visionneuse : connectée à « %s »", o.title.c_str());
		return;
	}
	// Émetteur perdu : horloge et pistes repartent de zéro à la reconnexion
	clock_.reset();
	have_phase_ = false;
	phase_ = 0;
	std::lock_guard lock(streams_mutex_);
	streams_.clear();
	rebind();
}

void Viewer::Impl::on_packet(const MediaPacket &pkt)
{
	if (pkt.codec == Codec::Meta) {
		std::lock_guard lock(streams_mutex_);
		on_meta(pkt.data);
		return;
	}

	const int64_t lag = (int64_t)(pkt.arrival_ns - pkt.pts_ns);
	if (is_video(pkt.codec)) {
		if (pkt.pid != video_pid_)
			return;
		clock_.observe(pkt.pts_ns, pkt.arrival_ns);
		max_video_lag_ = std::max(max_video_lag_, lag);
		{
			std::lock_guard lock(queue_mutex_);
			bool corrupt = pkt.corrupt;
			if (queue_.size() >= kMaxQueue) {
				// Le décodage n'a pas suivi : on jette tout et on repart d'une image clé
				queue_.clear();
				corrupt = true;
			}
			queue_.push_back({pkt.pid, pkt.codec, pkt.pts_ns, pkt.arrival_ns, {pkt.data.begin(), pkt.data.end()},
					  pkt.keyframe, corrupt});
		}
		queue_cv_.notify_one();
	} else if (pkt.codec == Codec::Opus) {
		max_audio_lag_ = std::max(max_audio_lag_, lag);
		on_audio(pkt);
	}

	if (pkt.arrival_ns - last_report_ >= 1'000'000'000ULL) {
		last_report_ = pkt.arrival_ns;
		measure::clock(clock_.offset_ns(), phase_.load(), max_video_lag_, max_audio_lag_);
		max_video_lag_ = max_audio_lag_ = INT64_MIN;
		const uint64_t n = display_count_.exchange(0), sum = display_sum_.exchange(0);
		if (n)
			receiver_.report_latency(sum / n);
	}
}

// ---------- Son ----------

void Viewer::Impl::on_audio(const MediaPacket &pkt)
{
	std::lock_guard lock(audio_mutex_);
	auto it = audio_.find(pkt.pid);
	if (it == audio_.end())
		return;
	// Le son n'est lu qu'une latence de sortie avant son heure : il doit être là avant
	clock_.observe(pkt.pts_ns, pkt.arrival_ns + out_.latency_ns());

	std::vector<std::span<const uint8_t>> packets;
	if (!split_opus_pes(pkt.data.data(), pkt.data.size(), packets))
		return;
	uint64_t pts = pkt.pts_ns;
	for (const auto &p : packets) {
		const int n = it->second->decoder.decode(p.data(), p.size(), pcm_.data(), 5760);
		if (n <= 0)
			continue;
		mixer_.write(it->second->writer, pts, pcm_.data(), n);
		pts += (uint64_t)n * 1'000'000'000ULL / kRate;
	}
}

// Sortie son : on lit le mélange à l'heure d'émetteur de l'instant où ces échantillons seront
// entendus, avec la phase de l'image pour rester calé sur ce qui est à l'écran.
void Viewer::Impl::fill(float *out, uint32_t frames, uint64_t first_ns)
{
	if (!clock_.ready()) {
		have_cursor_ = false;
		return;
	}
	const int64_t shift = clock_.offset_ns() + (video_pid_ ? phase_.load() : 0);
	const double want = ((double)first_ns - (double)shift) * kRate / 1e9;
	double err = cursor_ - want;
	if (!have_cursor_ || std::fabs(err) > kRate / 50.0) {
		cursor_ = want;
		err = 0;
		have_cursor_ = true;
	}
	// Rattrapage en douceur, 0,5 % de vitesse au plus
	double step = 1.0;
	if (std::fabs(err) > 2)
		step = 1.0 - std::clamp(err / (kRate * 0.5), -0.005, 0.005);
	mixer_.read(cursor_, step, out, frames);

	if (measure::enabled())
		detect_beep(out, frames, first_ns);
}

void Viewer::Impl::detect_beep(const float *samples, uint32_t frames, uint64_t first_ns)
{
	for (uint32_t i = 0; i < frames; i++) {
		if (std::fabs(samples[i * 2]) > 0.05f) {
			if (!in_beep_ && quiet_samples_ > kRate / 10)
				measure::beep(first_ns + (uint64_t)i * 1'000'000'000ULL / kRate);
			in_beep_ = true;
			quiet_samples_ = 0;
		} else if (++quiet_samples_ > kRate / 100) {
			in_beep_ = false;
		}
	}
}

// ---------- Décodage ----------

void Viewer::Impl::decode_loop()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	std::unique_ptr<VideoDecoder> decoder;
	Codec codec = Codec::Meta;
	uint16_t pid = 0;
	bool failed = false;
	bool waiting_keyframe = true;
	std::deque<Info> infos;
	uint64_t progress = 0; // dernière image sortie du décodeur (ou image clé donnée)

	for (;;) {
		Packet p;
		{
			std::unique_lock lock(queue_mutex_);
			queue_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
			if (stop_)
				break;
			p = std::move(queue_.front());
			queue_.pop_front();
		}
		if (!device_)
			continue;

		if (p.codec != codec || p.pid != pid) {
			codec = p.codec;
			pid = p.pid;
			decoder = create_mf_decoder(device_.Get(), codec);
			failed = !decoder;
			waiting_keyframe = true;
			cadence_ = CadenceSmoother();
			decoded_ = 0;
			infos.clear();
			progress = now_ns();
			std::lock_guard lock(info_mutex_);
			if (decoder) {
				decoder_name_ = std::string(decoder->name()) + (decoder->hardware() ? "" : " (processeur)");
				error_.clear();
			} else {
				error_ = codec == Codec::HEVC ? "Ce PC ne sait pas décoder le HEVC. Installez l'extension HEVC de "
								"Windows, ou passez l'émetteur en H.264."
					 : codec == Codec::AV1 ? "Ce PC ne sait pas décoder l'AV1. Installez l'extension AV1 de "
								 "Windows (« AV1 Video Extension », Microsoft Store), ou passez "
								 "l'émetteur en H.264."
							       : "Aucun décodeur H.264 sur ce PC.";
				FLOG_E("%s", error_.c_str());
			}
		}
		if (failed)
			continue;

		if (p.corrupt)
			request_keyframe();
		if (waiting_keyframe) {
			if (!p.keyframe) {
				request_keyframe();
				continue;
			}
			waiting_keyframe = false;
		}

		const int64_t time = (int64_t)(p.pts / 100);
		infos.push_back({time, p.pts, p.arrival});
		if (infos.size() > 16)
			infos.pop_front();
		const uint64_t before = matched_;
		const bool ok = decoder->decode(p.data.data(), p.data.size(), time,
						[&](const DecodedFrame &f) { on_frame(f, infos); });
		const uint64_t now = now_ns();
		if (matched_ != before || p.keyframe)
			progress = now;

		// Un décodeur qui refuse une image ou ne sort plus rien (référence perdue : le
		// décodeur AV1 de Windows ne s'en remet pas seul) est refait, et l'image repart de la
		// prochaine image clé
		if (!ok || now - progress > kStallNs) {
			FLOG_W("Visionneuse : %s, décodeur refait", ok ? "plus aucune image décodée" : "image refusée par le décodeur");
			decoder = create_mf_decoder(device_.Get(), codec);
			failed = !decoder;
			waiting_keyframe = true;
			infos.clear();
			progress = now;
			request_keyframe();
		}
	}

	decoder.reset();
	CoUninitialize();
}

int Viewer::Impl::take_slot(uint32_t w, uint32_t h)
{
	int s = -1;
	{
		std::lock_guard lock(ready_mutex_);
		for (int i = 0; i < kSlots; i++) {
			if (!slots_[i].busy) {
				s = i;
				break;
			}
		}
		if (s < 0 && !ready_.empty()) {
			s = ready_.front().slot; // plus de place : la plus ancienne image en attente est sautée
			ready_.pop_front();
			dropped_++;
		}
		if (s < 0)
			return -1;
		slots_[s].busy = true;
	}

	Slot &slot = slots_[s];
	if (!slot.tex || slot.w != w || slot.h != h) {
		slot.view.Reset();
		slot.tex.Reset();
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		// Comme les images du décodeur : le processeur vidéo les accepte en entrée
		desc.BindFlags = D3D11_BIND_DECODER;
		if (FAILED(device_->CreateTexture2D(&desc, nullptr, &slot.tex))) {
			free_slot(s);
			return -1;
		}
		slot.w = w;
		slot.h = h;
	}
	return s;
}

void Viewer::Impl::free_slot(int s)
{
	std::lock_guard lock(ready_mutex_);
	slots_[s].busy = false;
}

void Viewer::Impl::on_frame(const DecodedFrame &f, std::deque<Info> &infos)
{
	const uint64_t decoded_ns = now_ns();
	Info info{};
	bool found = false;
	while (!infos.empty()) {
		if (infos.front().time == f.time) {
			info = infos.front();
			infos.pop_front();
			found = true;
			break;
		}
		if (infos.front().time > f.time)
			break;
		infos.pop_front();
	}
	if (!found)
		return;
	matched_++;

	D3D11_TEXTURE2D_DESC d;
	f.texture->GetDesc(&d);
	if (d.Format != DXGI_FORMAT_NV12)
		return;
	const int s = take_slot(d.Width, d.Height);
	if (s < 0)
		return;
	ctx_->CopySubresourceRegion(slots_[s].tex.Get(), 0, 0, 0, 0, f.texture, f.subresource, nullptr);

	Ready r;
	r.slot = s;
	r.visible = f.visible;
	r.pts = info.pts;
	r.arrival = info.arrival;
	r.decoded = decoded_ns;

	// Mesure : la mire est relue dans une copie que le processeur peut lire
	if (measure::enabled()) {
		D3D11_TEXTURE2D_DESC pd{};
		if (probe_)
			probe_->GetDesc(&pd);
		if (!probe_ || pd.Width != d.Width || pd.Height != d.Height) {
			pd = {};
			pd.Width = d.Width;
			pd.Height = d.Height;
			pd.MipLevels = pd.ArraySize = 1;
			pd.Format = DXGI_FORMAT_NV12;
			pd.SampleDesc.Count = 1;
			pd.Usage = D3D11_USAGE_STAGING;
			pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			probe_.Reset();
			device_->CreateTexture2D(&pd, nullptr, &probe_);
		}
		D3D11_MAPPED_SUBRESOURCE map;
		if (probe_) {
			ctx_->CopyResource(probe_.Get(), slots_[s].tex.Get());
			if (SUCCEEDED(ctx_->Map(probe_.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				r.code = mire::read_code((const uint8_t *)map.pData, map.RowPitch, r.capture, r.number);
				r.flash = r.code && mire::flash_visible((const uint8_t *)map.pData, map.RowPitch);
				ctx_->Unmap(probe_.Get(), 0);
			}
		}
	}

	// Les premières images comptent l'initialisation du décodeur : hors de l'horloge
	const uint64_t pts = cadence_.smooth(info.pts);
	if (decoded_++ >= kWarmupFrames)
		clock_.observe(pts, decoded_ns);
	if (!clock_.ready()) {
		free_slot(s);
		return;
	}
	r.target = clock_.present(pts);

	{
		std::lock_guard lock(ready_mutex_);
		while (ready_.size() >= kMaxReady) {
			slots_[ready_.front().slot].busy = false;
			ready_.pop_front();
			dropped_++;
		}
		ready_.push_back(r);
	}
	ready_cv_.notify_one();
}

// ---------- Fenêtre ----------

LRESULT CALLBACK Viewer::Impl::wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE)
		SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
	auto *self = (Impl *)GetWindowLongPtrW(h, GWLP_USERDATA);
	return self ? self->handle(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT Viewer::Impl::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_SIZE:
		resized_ = true;
		ready_cv_.notify_all();
		return 0;
	case WM_LBUTTONDBLCLK:
		set_fullscreen(h, !fullscreen_);
		return 0;
	case WM_KEYDOWN:
		if (wp == VK_F11)
			set_fullscreen(h, !fullscreen_);
		else if (wp == VK_ESCAPE && fullscreen_)
			set_fullscreen(h, false);
		return 0;
	case WM_SYSKEYDOWN:
		if (wp == VK_RETURN) {
			set_fullscreen(h, !fullscreen_);
			return 0;
		}
		break;
	case WM_MOUSEMOVE:
		last_mouse_ = GetTickCount64();
		break;
	case WM_SETCURSOR:
		// En plein écran, le curseur disparaît au bout de 2 s sans bouger
		if (LOWORD(lp) == HTCLIENT && fullscreen_ && GetTickCount64() - last_mouse_ > 2000) {
			SetCursor(nullptr);
			return TRUE;
		}
		break;
	case WM_TIMER:
		if (fullscreen_ && GetTickCount64() - last_mouse_ > 2000) {
			POINT p;
			GetCursorPos(&p);
			if (WindowFromPoint(p) == h)
				SetCursor(nullptr);
		}
		return 0;
	case WM_CLOSE:
		// La fenêtre se ferme une fois l'affichage arrêté (fil de présentation)
		if (!stop_)
			closed_ = true;
		closing_ = true;
		ready_cv_.notify_all();
		return 0;
	case WM_VIEWER_DESTROY:
		DestroyWindow(h);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

void Viewer::Impl::set_fullscreen(HWND h, bool on)
{
	if (on == fullscreen_)
		return;
	if (on) {
		GetWindowPlacement(h, &placement_);
		MONITORINFO mi{sizeof(mi)};
		GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
			     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	} else {
		SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPlacement(h, &placement_);
		SetWindowPos(h, nullptr, 0, 0, 0, 0,
			     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	fullscreen_ = on;
	last_mouse_ = GetTickCount64();
}

void Viewer::Impl::window_loop()
{
	HINSTANCE inst = GetModuleHandleW(nullptr);
	static std::once_flag once;
	std::call_once(once, [inst] {
		WNDCLASSEXW wc{sizeof(wc)};
		wc.style = CS_DBLCLKS;
		wc.lpfnWndProc = wnd_proc;
		wc.hInstance = inst;
		wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_FLASH));
		wc.hIconSm = wc.hIcon;
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = kClass;
		RegisterClassExW(&wc);
	});

	// 1280×720 à l'échelle de l'écran, au centre de l'écran voulu (sinon celui de la souris)
	RECT area{0, 0, 1920, 1080};
	bool placed = false;
	if (o.screen > 0) {
		for (const ScreenInfo &s : list_screens()) {
			if (s.index == o.screen) {
				area = {s.x, s.y, s.x + s.width, s.y + s.height};
				placed = true;
			}
		}
	}
	if (!placed) {
		POINT p;
		GetCursorPos(&p);
		MONITORINFO mi{sizeof(mi)};
		if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &mi))
			area = mi.rcWork;
	}
	const UINT dpi = GetDpiForSystem();
	const int aw = area.right - area.left, ah = area.bottom - area.top;
	RECT r{0, 0, std::min(MulDiv(1280, dpi, 96), aw * 9 / 10), std::min(MulDiv(720, dpi, 96), ah * 9 / 10)};
	AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
	const int ww = r.right - r.left, wh = r.bottom - r.top;

	HWND h = CreateWindowExW(o.background ? WS_EX_NOACTIVATE : 0, kClass, to_wide(o.title).c_str(), WS_OVERLAPPEDWINDOW,
				 area.left + (aw - ww) / 2, area.top + (ah - wh) / 2, ww, wh, nullptr, nullptr, inst, this);
	if (!h) {
		std::lock_guard lock(info_mutex_);
		error_ = "Fenêtre impossible à créer";
		closed_ = true;
		return;
	}
	hwnd_ = h;
	ShowWindow(h, o.background ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
	if (o.fullscreen)
		set_fullscreen(h, true);
	SetTimer(h, 1, 500, nullptr);

	std::thread vblank(&Impl::vblank_loop, this);
	std::thread present(&Impl::present_loop, this);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	closing_ = true;
	ready_cv_.notify_all();
	present.join();
	vblank.join();
	hwnd_ = nullptr;
}

// ---------- Balayage de l'écran ----------

// Suit les balayages de l'écran où se trouve la fenêtre : heure du dernier et période
void Viewer::Impl::vblank_loop()
{
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(device_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)))
		return;
	HMONITOR current = nullptr;
	ComPtr<IDXGIOutput> output;
	uint64_t checked = 0;

	while (!closing_ && !stop_) {
		const uint64_t now = now_ns();
		if (now - checked > 500'000'000ULL) {
			checked = now;
			HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
			if (mon != current) {
				current = mon;
				output.Reset();
				ComPtr<IDXGIOutput> o2;
				for (UINT i = 0; adapter->EnumOutputs(i, &o2) != DXGI_ERROR_NOT_FOUND; i++) {
					DXGI_OUTPUT_DESC d;
					if (SUCCEEDED(o2->GetDesc(&d)) && d.Monitor == mon) {
						output = o2;
						break;
					}
					o2.Reset();
				}
				std::lock_guard lock(grid_mutex_);
				grid_ = period_ = last_vb_ = 0;
				warmup_.clear();
			}
		}
		// Écran branché sur une autre carte : pas de balayage connu, images remises à l'heure
		if (!output || FAILED(output->WaitForVBlank())) {
			Sleep(16);
			continue;
		}

		const uint64_t vb = now_ns();
		std::lock_guard lock(grid_mutex_);
		if (!period_) {
			// Période : médiane des premiers intervalles
			if (last_vb_ && vb - last_vb_ > 2'000'000ULL && vb - last_vb_ < 50'000'000ULL)
				warmup_.push_back(vb - last_vb_);
			if (warmup_.size() >= 9) {
				std::nth_element(warmup_.begin(), warmup_.begin() + 4, warmup_.end());
				period_ = warmup_[4];
				grid_ = vb;
			}
		} else {
			const uint64_t predicted = grid_ + period_;
			const int64_t err = (int64_t)(vb - predicted);
			if (std::llabs(err) < (int64_t)period_ / 4) {
				grid_ = (uint64_t)((int64_t)predicted + err / 8);
				period_ = (uint64_t)((int64_t)period_ + err / 64);
			} else {
				grid_ = vb; // balayage manqué : on se recale
			}
		}
		last_vb_ = vb;
	}
}

// ---------- Affichage ----------

bool Viewer::Impl::create_swap_chain()
{
	HWND h = hwnd_;
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIFactory2> factory;
	if (!device_ || FAILED(device_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
	    FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
		return false;
	RECT rc;
	GetClientRect(h, &rc);
	sw_w_ = (uint32_t)std::max<LONG>(rc.right, 16);
	sw_h_ = (uint32_t)std::max<LONG>(rc.bottom, 16);

	DXGI_SWAP_CHAIN_DESC1 sd{};
	sd.Width = sw_w_;
	sd.Height = sw_h_;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), h, &sd, nullptr, nullptr, &swap_)))
		return false;
	factory->MakeWindowAssociation(h, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

	ComPtr<ID3D11DeviceContext> ctx;
	device_->GetImmediateContext(&ctx);
	if (FAILED(device_.As(&vdev_)) || FAILED(ctx.As(&vctx_)))
		return false;

	if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf())) &&
	    SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
					  (IUnknown **)dwrite_.GetAddressOf()))) {
		const float size = 20.0f * GetDpiForWindow(h) / 96.0f;
		if (SUCCEEDED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
							DWRITE_FONT_STRETCH_NORMAL, size, L"fr-FR", &text_format_))) {
			text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		}
	}
	return true;
}

void Viewer::Impl::resize()
{
	RECT rc;
	GetClientRect(hwnd_, &rc);
	if (rc.right <= 0 || rc.bottom <= 0)
		return; // réduite dans la barre des tâches
	if ((uint32_t)rc.right == sw_w_ && (uint32_t)rc.bottom == sw_h_)
		return;
	out_views_.clear();
	if (SUCCEEDED(swap_->ResizeBuffers(0, (UINT)rc.right, (UINT)rc.bottom, DXGI_FORMAT_UNKNOWN, 0))) {
		sw_w_ = (uint32_t)rc.right;
		sw_h_ = (uint32_t)rc.bottom;
	}
	last_text_.clear();
}

bool Viewer::Impl::ensure_processor(uint32_t in_w, uint32_t in_h)
{
	if (vproc_ && in_w == proc_in_w_ && in_h == proc_in_h_ && sw_w_ == proc_out_w_ && sw_h_ == proc_out_h_)
		return true;
	vproc_.Reset();
	venum_.Reset();
	out_views_.clear();
	proc_gen_++;

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC c{};
	c.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	c.InputWidth = in_w;
	c.InputHeight = in_h;
	c.OutputWidth = sw_w_;
	c.OutputHeight = sw_h_;
	c.InputFrameRate = {60, 1};
	c.OutputFrameRate = {60, 1};
	c.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	HRESULT hr;
	if (FAILED(hr = vdev_->CreateVideoProcessorEnumerator(&c, &venum_)) ||
	    FAILED(hr = vdev_->CreateVideoProcessor(venum_.Get(), 0, &vproc_))) {
		FLOG_W("Visionneuse : processeur vidéo impossible (%ux%u vers %ux%u, 0x%08lx)", in_w, in_h, sw_w_, sw_h_,
		       (unsigned long)hr);
		vproc_.Reset();
		return false;
	}
	// YUV BT.709 « studio » de l'encodeur vers le RVB plein de l'écran, fond noir
	vctx_->VideoProcessorSetStreamColorSpace1(vproc_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
	vctx_->VideoProcessorSetOutputColorSpace1(vproc_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	vctx_->VideoProcessorSetStreamAutoProcessingMode(vproc_.Get(), 0, FALSE);
	vctx_->VideoProcessorSetStreamFrameFormat(vproc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	D3D11_VIDEO_COLOR black{};
	black.RGBA = {0.0f, 0.0f, 0.0f, 1.0f};
	vctx_->VideoProcessorSetOutputBackgroundColor(vproc_.Get(), FALSE, &black);
	proc_in_w_ = in_w;
	proc_in_h_ = in_h;
	proc_out_w_ = sw_w_;
	proc_out_h_ = sw_h_;
	return true;
}

// Heure de présentation : le premier balayage à partir de l'heure visée, sauf à la frontière
// entre deux balayages, où l'on garde le côté choisi pour l'image précédente tant qu'on reste
// à moins de 1,5 ms (sinon la moindre gigue fait alterner les images). L'image est remise au
// milieu de l'intervalle qui précède ce balayage.
void Viewer::Impl::plan(Ready &f)
{
	uint64_t grid, period;
	{
		std::lock_guard lock(grid_mutex_);
		grid = grid_;
		period = period_;
	}
	const uint64_t now = now_ns();
	f.planned = true;
	if (!period) {
		f.show = f.release = std::max(f.target, now);
		return;
	}

	uint64_t show = f.target <= grid ? grid : grid + (f.target - grid + period - 1) / period * period;
	const int64_t d = (int64_t)(show - f.target);
	if (have_prev_) {
		const int64_t span = (int64_t)period;
		int64_t best = d;
		for (const int64_t cand : {d - span, d + span}) {
			if (cand >= -(int64_t)kGuardNs && cand < span + (int64_t)kGuardNs &&
			    std::llabs(cand - prev_d_) < std::llabs(best - prev_d_))
				best = cand;
		}
		show = (uint64_t)((int64_t)f.target + best);
	}
	while (show < now + kGuardNs)
		show += period;
	prev_d_ = (int64_t)(show - f.target);
	have_prev_ = true;
	f.show = show;
	f.release = std::max(show - period / 2, now);
}

bool Viewer::Impl::draw(const Ready &f)
{
	// Chaque cause d'échec n'est notée qu'une fois
	auto fail = [this](int step, HRESULT hr) {
		if (!(failures_ & (1u << step))) {
			failures_ |= 1u << step;
			FLOG_W("Visionneuse : affichage impossible (étape %d, 0x%08lx)", step, (unsigned long)hr);
		}
		return false;
	};
	Slot &slot = slots_[f.slot];
	if (!slot.tex)
		return fail(0, E_POINTER);
	if (!ensure_processor(slot.w, slot.h))
		return fail(1, E_FAIL);
	HRESULT hr;
	if (!slot.view || slot.view_gen != proc_gen_) {
		slot.view.Reset();
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
		ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		if (FAILED(hr = vdev_->CreateVideoProcessorInputView(slot.tex.Get(), venum_.Get(), &ivd, &slot.view)))
			return fail(2, hr);
		slot.view_gen = proc_gen_;
	}

	ComPtr<ID3D11Texture2D> back;
	if (FAILED(hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back))))
		return fail(3, hr);
	auto it = out_views_.find(back.Get());
	if (it == out_views_.end()) {
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
		ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11VideoProcessorOutputView> view;
		if (FAILED(hr = vdev_->CreateVideoProcessorOutputView(back.Get(), venum_.Get(), &ovd, &view)))
			return fail(4, hr);
		it = out_views_.emplace(back.Get(), view).first;
	}

	const RECT dst = fit_rect(f.visible, sw_w_, sw_h_);
	vctx_->VideoProcessorSetStreamSourceRect(vproc_.Get(), 0, TRUE, &f.visible);
	vctx_->VideoProcessorSetStreamDestRect(vproc_.Get(), 0, TRUE, &dst);
	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.pInputSurface = slot.view.Get();
	if (FAILED(hr = vctx_->VideoProcessorBlt(vproc_.Get(), it->second.Get(), 0, 1, &stream)))
		return fail(5, hr);
	if (FAILED(hr = swap_->Present(0, 0)))
		return fail(6, hr);
	return true;
}

void Viewer::Impl::draw_text(const std::wstring &text)
{
	ComPtr<IDXGISurface> surface;
	ComPtr<ID2D1RenderTarget> rt;
	if (d2d_ && text_format_ && SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
		const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
			96.0f, 96.0f);
		d2d_->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &rt);
	}
	if (rt) {
		ComPtr<ID2D1SolidColorBrush> brush;
		rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.80f, 0.83f), &brush);
		rt->BeginDraw();
		rt->Clear(D2D1::ColorF(0.06f, 0.07f, 0.08f));
		const D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)sw_w_, (float)sw_h_);
		rt->DrawText(text.c_str(), (UINT32)text.size(), text_format_.Get(), rect, brush.Get());
		rt->EndDraw();
	}
	rt.Reset();
	surface.Reset();
	swap_->Present(0, 0);
}

std::wstring Viewer::Impl::status_text()
{
	{
		std::lock_guard lock(info_mutex_);
		if (!error_.empty())
			return to_wide(error_);
	}
	const std::wstring who = to_wide(o.title);
	if (!receiver_.connected())
		return L"Connexion à " + who + L"…";
	if (o.video == "-")
		return who + L" : son seulement";
	if (!video_pid_) {
		if (names_known_ && !o.video.empty())
			return L"La piste « " + to_wide(o.video) + L" » n'est pas envoyée par " + who;
		return who + L" n'envoie pas d'image";
	}
	return L"En attente de l'image…";
}

// Image montrée à l'écran : phase pour le son, latence, journal de mesure
void Viewer::Impl::frame_shown(const Ready &f, uint64_t shown_ns)
{
	const int64_t sample = (int64_t)(shown_ns - f.target);
	if (!have_phase_) {
		phase_ = sample;
		have_phase_ = true;
	} else {
		const int64_t p = phase_.load();
		phase_ = p + (sample - p) / 32;
	}
	if (shown_ns > f.pts) {
		latency_sum_ += shown_ns - f.pts;
		latency_n_++;
	}
	if (f.arrival && shown_ns > f.arrival) {
		display_sum_ += shown_ns - f.arrival;
		display_count_++;
	}
	measure::shown(f.target, shown_ns);
}

void Viewer::Impl::present_loop()
{
	if (!create_swap_chain()) {
		{
			std::lock_guard lock(info_mutex_);
			error_ = "Affichage impossible sur cet écran";
		}
		while (!closing_ && !stop_)
			Sleep(50);
		PostMessageW(hwnd_, WM_VIEWER_DESTROY, 0, 0);
		return;
	}

	while (!closing_ && !stop_) {
		if (resized_.exchange(false))
			resize();

		// Prochaine image à présenter
		Ready f;
		bool have = false;
		{
			std::unique_lock lock(ready_mutex_);
			for (Ready &r : ready_) {
				if (!r.planned)
					plan(r);
			}
			const uint64_t now = now_ns();
			if (ready_.empty() || ready_.front().release > now) {
				const uint64_t until = ready_.empty() ? now + 50'000'000ULL : ready_.front().release;
				if (until - now > 2'000'000ULL) {
					ready_cv_.wait_for(lock, std::chrono::nanoseconds(until - now - 1'000'000ULL));
				} else {
					lock.unlock();
					sleep_until_ns(until);
				}
			} else {
				// La dernière image due ; celles d'avant sont sautées
				while (!ready_.empty() && ready_.front().release <= now) {
					if (have) {
						slots_[f.slot].busy = false;
						dropped_++;
					}
					f = ready_.front();
					ready_.pop_front();
					have = true;
				}
			}
		}

		if (!have) {
			// Pas d'image depuis un moment : un message à la place
			const uint64_t now = now_ns();
			if (now - last_present_ > 500'000'000ULL) {
				const std::wstring text = status_text();
				if (text != last_text_ || now - last_present_ > 1'000'000'000ULL) {
					draw_text(text);
					last_text_ = text;
					last_present_ = now;
				}
			}
			continue;
		}

		const uint64_t released = now_ns();
		const bool ok = draw(f);
		free_slot(f.slot);
		if (!ok)
			continue;
		last_present_ = released;
		last_text_.clear();
		shown_++;
		if (f.code)
			measure::frame(f.number, f.capture, f.arrival, f.decoded, released, f.target, f.flash, f.pts);

		// Heure d'affichage : le balayage prévu, corrigé de l'écart moyen avec l'heure réelle que
		// Windows donne pour une partie des images (composition du bureau comprise)
		UINT count = 0;
		if (SUCCEEDED(swap_->GetLastPresentCount(&count)))
			pending_[count] = {f.target, f.show, f.pts};
		DXGI_FRAME_STATISTICS st{};
		if (SUCCEEDED(swap_->GetFrameStatistics(&st)) && st.SyncQPCTime.QuadPart) {
			auto it = pending_.find(st.PresentCount);
			if (it != pending_.end()) {
				const int64_t c = (int64_t)(qpc_to_ns(st.SyncQPCTime.QuadPart) - it->second.show);
				correction_ = stats_ok_ ? correction_ + (c - correction_) / 16 : c;
				stats_ok_ = true;
			}
			pending_.erase(pending_.begin(), pending_.upper_bound(st.PresentCount));
		}
		while (pending_.size() > 16)
			pending_.erase(pending_.begin());
		frame_shown(f, (uint64_t)((int64_t)f.show + correction_));
	}

	out_views_.clear();
	for (Slot &s : slots_)
		s.view.Reset();
	vproc_.Reset();
	venum_.Reset();
	text_format_.Reset();
	dwrite_.Reset();
	d2d_.Reset();
	swap_.Reset();
	PostMessageW(hwnd_, WM_VIEWER_DESTROY, 0, 0);
}

// ---------- État ----------

json Viewer::Impl::status()
{
	const uint64_t now = now_ns();
	const double secs = stats_at_ ? (now - stats_at_) / 1e9 : 0;
	stats_at_ = now;
	const uint64_t shown = shown_.exchange(0), dropped = dropped_.exchange(0);
	const uint64_t lat_sum = latency_sum_.exchange(0), lat_n = latency_n_.exchange(0);

	json j;
	std::string error, decoder, note;
	{
		std::lock_guard lock(info_mutex_);
		error = error_;
		decoder = decoder_name_;
		note = note_;
	}
	std::string state;
	if (!error.empty())
		state = "erreur";
	else if (!receiver_.connected())
		state = "connexion";
	else if (!video_pid_)
		state = o.video == "-" ? "son" : "sans_image";
	else
		state = shown ? "lecture" : "attente";
	j["etat"] = state;
	j["message"] = error;
	j["remarque"] = note;
	j["ips"] = secs > 0 ? shown / secs : 0;
	j["sautees"] = dropped;
	const SrtLinkStats st = receiver_.stats();
	j["debit"] = st.mbps;
	j["rtt"] = st.rtt_ms;
	j["pertes"] = st.lost;
	j["decodeur"] = decoder;
	if (!o.audio.empty())
		j["sortie"] = out_ok_ ? out_.device_name() : "introuvable";
	// Sur le même PC, émetteur et visionneuse lisent la même horloge : latence de bout en bout
	if (local_ && lat_n)
		j["latence_ms"] = lat_sum / (double)lat_n / 1e6;
	return j;
}

// ---------- Viewer ----------

Viewer::Viewer(const ViewerOptions &options) : impl_(std::make_unique<Impl>(options)) {}

Viewer::~Viewer() = default;

bool Viewer::closed() const
{
	return impl_->closed_;
}

json Viewer::status()
{
	return impl_->status();
}
