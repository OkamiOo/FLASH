#include "provider.h"

#include <flash/clock.h>
#include <flash/log.h>
#include <flash/obs_link.h>

#include <obs-module.h>
#include <graphics/vec4.h>

extern "C" {
#include <libswresample/swresample.h>
}

#include <nlohmann/json.hpp>

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using json = nlohmann::json;
namespace ol = flash::obslink;

namespace provider {

namespace {

constexpr uint32_t kPreviewW = 320, kPreviewH = 180;
constexpr uint64_t kPreviewEveryNs = 250'000'000ULL;
constexpr size_t kMaxQueuedBytes = 32u << 20;

// Une scène, une source ou le programme, rendu à la taille d'envoi dans trois textures
// partagées à verrou à clé
struct VideoStream {
	uint32_t id = 0;
	bool program = false;
	obs_source_t *source = nullptr;
	obs_view_t *view = nullptr;
	uint32_t width = 0, height = 0;
	gs_texture_t *tex[3] = {};
	uint32_t next = 0;
};

struct AudioStream {
	uint32_t id = 0;
	obs_source_t *source = nullptr; // nul : le mix d'OBS
	SwrContext *swr = nullptr;
	std::vector<float> out;
};

class Provider {
public:
	void start();
	void stop();

private:
	void serve();
	void writer();
	void send(std::vector<uint8_t> bytes, bool droppable = false);
	void on_message(const ol::Message &m);
	void open_video(const json &o);
	void open_audio(const json &o);
	void close_stream(uint32_t id);
	void close_all();
	std::string catalog();

	static void rendered(void *param);
	void on_rendered();
	void draw(gs_texture_t *target, uint32_t w, uint32_t h, const VideoStream &s, const struct obs_video_info &ovi);
	void preview_step(const struct obs_video_info &ovi);

	static void source_audio(void *param, obs_source_t *, const struct audio_data *data, bool muted);
	static void mix_audio(void *param, size_t, struct audio_data *data);
	void push_audio(AudioStream &a, const float *const *planes, uint32_t channels, uint32_t frames, uint32_t rate,
			uint64_t ts, bool interleaved);

	std::thread serve_thread_, writer_thread_;
	HANDLE stop_event_ = nullptr;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> connected_{false};
	std::shared_ptr<ol::Link> link_; // out_mutex_

	std::mutex out_mutex_;
	std::condition_variable out_cv_;
	std::deque<std::pair<std::vector<uint8_t>, bool>> out_;
	size_t out_bytes_ = 0;
	std::string last_catalog_;

	std::mutex streams_mutex_;
	std::map<uint32_t, std::unique_ptr<VideoStream>> video_;
	std::map<uint32_t, std::unique_ptr<AudioStream>> audio_;

	// Aperçus : une scène ou source tous les 250 ms, lue deux rendus plus tard (sans attendre le GPU)
	std::atomic<bool> previews_{false};
	std::vector<std::string> preview_names_; // streams_mutex_
	size_t preview_turn_ = 0;
	uint64_t last_preview_ = 0;
	gs_texture_t *preview_tex_ = nullptr;
	gs_stagesurf_t *preview_stage_ = nullptr;
	std::string staged_name_;
	int staged_frames_ = -1;
};

Provider g_provider;

void Provider::start()
{
	stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	stopping_ = false;
	obs_add_main_rendered_callback(rendered, this);
	serve_thread_ = std::thread(&Provider::serve, this);
	writer_thread_ = std::thread(&Provider::writer, this);
}

void Provider::stop()
{
	if (!stop_event_)
		return;
	stopping_ = true;
	SetEvent(stop_event_);
	out_cv_.notify_all();
	serve_thread_.join();
	writer_thread_.join();
	obs_remove_main_rendered_callback(rendered, this);
	close_all();
	obs_enter_graphics();
	gs_texture_destroy(preview_tex_);
	gs_stagesurface_destroy(preview_stage_);
	obs_leave_graphics();
	preview_tex_ = nullptr;
	preview_stage_ = nullptr;
	CloseHandle(stop_event_);
	stop_event_ = nullptr;
}

// ---------- Tube ----------

// Le tube n'est ouvert qu'à l'utilisateur qui fait tourner OBS
void Provider::serve()
{
	PSECURITY_DESCRIPTOR sd = nullptr;
	ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &sd, nullptr);
	SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
	bool warned = false;

	while (!stopping_) {
		HANDLE pipe = CreateNamedPipeW(ol::kPipe,
					       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
					       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
					       1u << 20, 1u << 16, 0, sd ? &sa : nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			// Un autre OBS sert déjà l'appli sur ce PC
			if (!warned)
				FLOG_I("Envoi depuis OBS : un autre OBS sert déjà l'appli FLASH");
			warned = true;
			WaitForSingleObject(stop_event_, 2000);
			continue;
		}

		OVERLAPPED ov{};
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		bool connected = ConnectNamedPipe(pipe, &ov) != 0;
		if (!connected) {
			const DWORD err = GetLastError();
			if (err == ERROR_PIPE_CONNECTED) {
				connected = true;
			} else if (err == ERROR_IO_PENDING) {
				HANDLE waits[2] = {ov.hEvent, stop_event_};
				DWORD done = 0;
				if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
					connected = GetOverlappedResult(pipe, &ov, &done, FALSE) != 0;
				else
					CancelIoEx(pipe, &ov);
			}
		}
		CloseHandle(ov.hEvent);

		if (connected && !stopping_) {
			FLOG_I("Envoi depuis OBS : appli FLASH connectée");
			{
				std::lock_guard lock(out_mutex_);
				link_ = std::make_shared<ol::Link>(pipe, stop_event_);
				out_.clear();
				out_bytes_ = 0;
				last_catalog_.clear();
			}
			connected_ = true;
			out_cv_.notify_all();

			ol::Message m;
			std::shared_ptr<ol::Link> l;
			{
				std::lock_guard lock(out_mutex_);
				l = link_;
			}
			while (!stopping_ && l->read(m))
				on_message(m);

			connected_ = false;
			previews_ = false;
			close_all();
			{
				std::lock_guard lock(out_mutex_);
				link_.reset();
			}
			FLOG_I("Envoi depuis OBS : appli FLASH déconnectée");
		}
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
	}
	LocalFree(sd);
}

void Provider::send(std::vector<uint8_t> bytes, bool droppable)
{
	if (!connected_)
		return;
	{
		std::lock_guard lock(out_mutex_);
		// L'appli ne lit plus assez vite : les aperçus attendront, jamais les images ni le son
		if (droppable && out_bytes_ > kMaxQueuedBytes / 4)
			return;
		out_bytes_ += bytes.size();
		out_.emplace_back(std::move(bytes), droppable);
	}
	out_cv_.notify_one();
}

// Écritures vers l'appli, et la liste des scènes et sources, renvoyée quand elle change
void Provider::writer()
{
	uint64_t last_catalog_check = 0;
	while (!stopping_) {
		std::vector<uint8_t> bytes;
		std::shared_ptr<ol::Link> l;
		{
			std::unique_lock lock(out_mutex_);
			out_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_ || !out_.empty(); });
			if (stopping_)
				break;
			l = link_;
			if (!out_.empty()) {
				bytes = std::move(out_.front().first);
				out_bytes_ -= bytes.size();
				out_.pop_front();
			}
		}
		if (l && !bytes.empty())
			l->write(bytes);

		const uint64_t now = flash::now_ns();
		if (connected_ && now - last_catalog_check > 1'000'000'000ULL) {
			last_catalog_check = now;
			const std::string c = catalog();
			bool changed;
			{
				std::lock_guard lock(out_mutex_);
				changed = c != last_catalog_;
				last_catalog_ = c;
			}
			if (changed)
				send(ol::pack(ol::Catalog, c));
		}
	}
}

// ---------- Demandes de l'appli ----------

void Provider::on_message(const ol::Message &m)
{
	const json j = m.type == ol::Open || m.type == ol::Close || m.type == ol::OpenAudio || m.type == ol::Previews
			       ? json::parse(m.body.begin(), m.body.end(), nullptr, false)
			       : json();
	switch (m.type) {
	case ol::Open:
		open_video(j);
		break;
	case ol::OpenAudio:
		open_audio(j);
		break;
	case ol::Close:
		close_stream(j.value("id", 0u));
		break;
	case ol::Previews:
		previews_ = j.value("actif", false);
		break;
	default:
		break;
	}
}

std::string Provider::catalog()
{
	struct obs_video_info ovi {};
	obs_get_video_info(&ovi);
	json j = {{"scenes", json::array()}, {"sources", json::array()}, {"base", {ovi.base_width, ovi.base_height}}};
	std::vector<std::string> names{""}; // nom vide : le programme
	auto ctx = std::make_pair(&j, &names);
	obs_enum_scenes(
		[](void *p, obs_source_t *s) {
			auto *c = (decltype(ctx) *)p;
			const char *name = obs_source_get_name(s);
			if (name && *name) {
				(*c->first)["scenes"].push_back({{"nom", name}});
				c->second->push_back(name);
			}
			return true;
		},
		&ctx);
	obs_enum_sources(
		[](void *p, obs_source_t *s) {
			auto *c = (decltype(ctx) *)p;
			const char *name = obs_source_get_name(s);
			const uint32_t flags = obs_source_get_output_flags(s);
			const char *id = obs_source_get_id(s);
			// Les sources FLASH elles-mêmes ne sont pas proposées : elles renverraient ce qu'on reçoit
			if (!name || !*name || (id && std::strncmp(id, "flash_", 6) == 0))
				return true;
			const bool video = (flags & OBS_SOURCE_VIDEO) != 0, audio = (flags & OBS_SOURCE_AUDIO) != 0;
			if (!video && !audio)
				return true;
			(*c->first)["sources"].push_back({{"nom", name},
							  {"video", video},
							  {"audio", audio},
							  {"largeur", obs_source_get_width(s)},
							  {"hauteur", obs_source_get_height(s)}});
			if (video)
				c->second->push_back(name);
			return true;
		},
		&ctx);
	{
		std::lock_guard lock(streams_mutex_);
		preview_names_ = std::move(names);
	}
	return j.dump();
}

void Provider::open_video(const json &o)
{
	auto s = std::make_unique<VideoStream>();
	s->id = o.value("id", 0u);
	s->program = o.value("type", "") == "programme";
	s->width = std::clamp(o.value("largeur", 1920u), 16u, 4096u) & ~1u;
	s->height = std::clamp(o.value("hauteur", 1080u), 16u, 4096u) & ~1u;
	if (!s->program) {
		s->source = obs_get_source_by_name(o.value("nom", "").c_str());
		if (!s->source) {
			send(ol::pack(ol::Closed, json({{"id", s->id}}).dump()));
			return;
		}
		// Une vue active la scène ou la source comme un projecteur : médias et navigateurs
		// tournent même hors du programme
		s->view = obs_view_create();
		obs_view_set_source(s->view, 0, s->source);
	}

	json handles = json::array();
	bool ok = true;
	obs_enter_graphics();
	for (auto &t : s->tex) {
		// Format typé (UNORM) : le processeur vidéo de l'appli le lit tel quel, sans copie
		t = gs_texture_create(s->width, s->height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET | GS_SHARED_KM_TEX);
		const uint32_t h = t ? gs_texture_get_shared_handle(t) : 0;
		ok = ok && h != 0;
		handles.push_back(h);
	}
	obs_leave_graphics();
	if (!ok) {
		FLOG_E("Envoi depuis OBS : textures partagées impossibles");
		send(ol::pack(ol::Closed, json({{"id", s->id}}).dump()));
		obs_enter_graphics();
		for (auto &t : s->tex)
			gs_texture_destroy(t);
		obs_leave_graphics();
		if (s->view) {
			obs_view_set_source(s->view, 0, nullptr);
			obs_view_destroy(s->view);
		}
		obs_source_release(s->source);
		return;
	}

	const uint32_t id = s->id;
	const json reply = {{"id", id}, {"largeur", s->width}, {"hauteur", s->height}, {"poignees", handles}};
	FLOG_I("Envoi depuis OBS : « %s » en %ux%u", s->program ? "programme" : o.value("nom", "").c_str(), s->width, s->height);
	{
		std::lock_guard lock(streams_mutex_);
		video_[id] = std::move(s);
	}
	send(ol::pack(ol::Opened, reply.dump()));
}

void Provider::open_audio(const json &o)
{
	auto a = std::make_unique<AudioStream>();
	a->id = o.value("id", 0u);
	const std::string name = o.value("nom", "");
	AudioStream *ap = a.get();
	if (name.empty()) {
		{
			std::lock_guard lock(streams_mutex_);
			audio_[ap->id] = std::move(a);
		}
		struct audio_convert_info conv {};
		conv.format = AUDIO_FORMAT_FLOAT;
		conv.samples_per_sec = 48000;
		conv.speakers = SPEAKERS_STEREO;
		obs_add_raw_audio_callback(0, &conv, mix_audio, ap);
		return;
	}
	a->source = obs_get_source_by_name(name.c_str());
	if (!a->source) {
		send(ol::pack(ol::Closed, json({{"id", a->id}}).dump()));
		return;
	}
	{
		std::lock_guard lock(streams_mutex_);
		audio_[ap->id] = std::move(a);
	}
	obs_source_add_audio_capture_callback(ap->source, source_audio, ap);
}

void Provider::close_stream(uint32_t id)
{
	std::unique_ptr<VideoStream> v;
	std::unique_ptr<AudioStream> a;
	{
		std::lock_guard lock(streams_mutex_);
		if (auto it = video_.find(id); it != video_.end()) {
			v = std::move(it->second);
			video_.erase(it);
		}
		if (auto it = audio_.find(id); it != audio_.end()) {
			a = std::move(it->second);
			audio_.erase(it);
		}
	}
	if (v) {
		obs_enter_graphics();
		for (auto &t : v->tex)
			gs_texture_destroy(t);
		obs_leave_graphics();
		if (v->view) {
			obs_view_set_source(v->view, 0, nullptr);
			obs_view_destroy(v->view);
		}
		obs_source_release(v->source);
	}
	if (a) {
		// Plus aucun rappel après le retrait : la piste peut être libérée
		if (a->source)
			obs_source_remove_audio_capture_callback(a->source, source_audio, a.get());
		else
			obs_remove_raw_audio_callback(0, mix_audio, a.get());
		obs_source_release(a->source);
		swr_free(&a->swr);
	}
}

void Provider::close_all()
{
	std::vector<uint32_t> ids;
	{
		std::lock_guard lock(streams_mutex_);
		for (auto &[id, v] : video_)
			ids.push_back(id);
		for (auto &[id, a] : audio_)
			ids.push_back(id);
	}
	for (uint32_t id : ids)
		close_stream(id);
}

// ---------- Rendu (fil graphique d'OBS) ----------

void Provider::rendered(void *param)
{
	((Provider *)param)->on_rendered();
}

void Provider::draw(gs_texture_t *target, uint32_t w, uint32_t h, const VideoStream &s, const struct obs_video_info &ovi)
{
	uint32_t cw = ovi.base_width, ch = ovi.base_height;
	if (s.source && !s.program && !obs_source_is_scene(s.source)) {
		cw = obs_source_get_width(s.source);
		ch = obs_source_get_height(s.source);
	}
	if (!cw || !ch)
		return;

	// Image centrée, proportions gardées
	const double scale = std::min((double)w / cw, (double)h / ch);
	const int vw = (int)(cw * scale), vh = (int)(ch * scale);

	gs_texture_t *prev = gs_get_render_target();
	gs_zstencil_t *prev_z = gs_get_zstencil_target();
	const enum gs_color_space prev_space = gs_get_color_space();
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_framebuffer_srgb(false);

	gs_set_render_target_with_color_space(target, nullptr, GS_CS_SRGB);
	gs_set_viewport(0, 0, (int)w, (int)h);
	struct vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	gs_set_viewport(((int)w - vw) / 2, ((int)h - vh) / 2, vw, vh);
	gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);

	if (s.program) {
		if (gs_texture_t *main = obs_get_main_texture()) {
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), main);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(main, 0, cw, ch);
		}
	} else if (s.view) {
		obs_view_render(s.view);
	} else if (s.source) {
		obs_source_video_render(s.source);
	}

	gs_set_render_target_with_color_space(prev, prev_z, prev_space);
	gs_enable_framebuffer_srgb(prev_srgb);
	gs_blend_state_pop();
	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

void Provider::on_rendered()
{
	if (!connected_)
		return;
	std::lock_guard lock(streams_mutex_);
	if (video_.empty() && !previews_)
		return;
	struct obs_video_info ovi {};
	obs_get_video_info(&ovi);
	const uint64_t t = obs_get_video_frame_time();

	for (auto &[id, s] : video_) {
		// La prochaine texture que l'appli a rendue (clé 0) ; aucune : l'image est sautée
		int i = -1;
		for (uint32_t k = 0; k < 3 && i < 0; k++) {
			const uint32_t idx = (s->next + k) % 3;
			if (gs_texture_acquire_sync(s->tex[idx], 0, 0) == 0)
				i = (int)idx;
		}
		if (i < 0)
			continue;
		draw(s->tex[i], s->width, s->height, *s, ovi);
		gs_texture_release_sync(s->tex[i], 1);
		s->next = (uint32_t)(i + 1) % 3;

		uint8_t body[16];
		const uint32_t index = (uint32_t)i;
		std::memcpy(body, &id, 4);
		std::memcpy(body + 4, &index, 4);
		std::memcpy(body + 8, &t, 8);
		send(ol::pack(ol::Frame, body, sizeof(body)));
	}

	if (previews_)
		preview_step(ovi);
}

void Provider::preview_step(const struct obs_video_info &ovi)
{
	if (!preview_tex_) {
		preview_tex_ = gs_texture_create(kPreviewW, kPreviewH, GS_BGRA, 1, nullptr, GS_RENDER_TARGET);
		preview_stage_ = gs_stagesurface_create(kPreviewW, kPreviewH, GS_BGRA);
		if (!preview_tex_ || !preview_stage_)
			return;
	}

	// Lecture de l'aperçu rendu deux images plus tôt : le GPU a fini, rien n'attend
	if (staged_frames_ >= 0 && ++staged_frames_ >= 2) {
		uint8_t *data = nullptr;
		uint32_t pitch = 0;
		if (gs_stagesurface_map(preview_stage_, &data, &pitch)) {
			std::vector<uint8_t> body;
			const uint16_t len = (uint16_t)std::min<size_t>(staged_name_.size(), 0xFFFF);
			const uint16_t w = kPreviewW, h = kPreviewH;
			body.resize(2 + len + 4 + (size_t)w * h * 4);
			std::memcpy(body.data(), &len, 2);
			std::memcpy(body.data() + 2, staged_name_.data(), len);
			std::memcpy(body.data() + 2 + len, &w, 2);
			std::memcpy(body.data() + 4 + len, &h, 2);
			uint8_t *dst = body.data() + 6 + len;
			for (uint32_t y = 0; y < h; y++)
				std::memcpy(dst + (size_t)y * w * 4, data + (size_t)y * pitch, (size_t)w * 4);
			gs_stagesurface_unmap(preview_stage_);
			send(ol::pack(ol::Preview, body.data(), body.size()), true);
		}
		staged_frames_ = -1;
	}

	const uint64_t now = flash::now_ns();
	if (staged_frames_ >= 0 || preview_names_.empty() || now - last_preview_ < kPreviewEveryNs)
		return;
	last_preview_ = now;
	const std::string name = preview_names_[preview_turn_++ % preview_names_.size()];
	VideoStream s;
	s.program = name.empty();
	if (!s.program && !(s.source = obs_get_source_by_name(name.c_str())))
		return;
	draw(preview_tex_, kPreviewW, kPreviewH, s, ovi);
	gs_stage_texture(preview_stage_, preview_tex_);
	obs_source_release(s.source);
	staged_name_ = name;
	staged_frames_ = 0;
}

// ---------- Son ----------

void Provider::push_audio(AudioStream &a, const float *const *planes, uint32_t channels, uint32_t frames, uint32_t rate,
			  uint64_t ts, bool interleaved)
{
	if (!frames || !channels)
		return;
	uint32_t out_frames = frames;
	a.out.resize((size_t)frames * 2 + 64);
	if (interleaved) {
		std::memcpy(a.out.data(), planes[0], (size_t)frames * 2 * sizeof(float));
	} else if (rate == 48000) {
		const float *l = planes[0], *r = planes[channels > 1 ? 1 : 0];
		for (uint32_t i = 0; i < frames; i++) {
			a.out[i * 2] = l[i];
			a.out[i * 2 + 1] = r[i];
		}
	} else {
		// OBS réglé sur une autre fréquence : ramené à 48 kHz
		if (!a.swr) {
			AVChannelLayout in, out;
			av_channel_layout_default(&in, (int)channels);
			av_channel_layout_default(&out, 2);
			if (swr_alloc_set_opts2(&a.swr, &out, AV_SAMPLE_FMT_FLT, 48000, &in, AV_SAMPLE_FMT_FLTP, (int)rate, 0,
						nullptr) < 0 ||
			    swr_init(a.swr) < 0) {
				swr_free(&a.swr);
				return;
			}
		}
		const int max_out = swr_get_out_samples(a.swr, (int)frames);
		a.out.resize((size_t)std::max(max_out, 1) * 2);
		uint8_t *out_planes[1] = {(uint8_t *)a.out.data()};
		const int n = swr_convert(a.swr, out_planes, max_out, (const uint8_t **)planes, (int)frames);
		if (n <= 0)
			return;
		out_frames = (uint32_t)n;
	}

	std::vector<uint8_t> body(16 + (size_t)out_frames * 8);
	std::memcpy(body.data(), &a.id, 4);
	std::memcpy(body.data() + 4, &ts, 8);
	std::memcpy(body.data() + 12, &out_frames, 4);
	std::memcpy(body.data() + 16, a.out.data(), (size_t)out_frames * 8);
	send(ol::pack(ol::Audio, body.data(), body.size()));
}

void Provider::source_audio(void *param, obs_source_t *, const struct audio_data *data, bool muted)
{
	auto *a = (AudioStream *)param;
	const struct audio_output_info *info = audio_output_get_info(obs_get_audio());
	if (!info)
		return;
	const uint32_t channels = get_audio_channels(info->speakers);
	if (muted) {
		// Coupée dans OBS : du silence, à la même heure
		std::vector<float> zero(data->frames, 0.0f);
		const float *planes[2] = {zero.data(), zero.data()};
		g_provider.push_audio(*a, planes, 2, data->frames, info->samples_per_sec, data->timestamp, false);
		return;
	}
	g_provider.push_audio(*a, (const float *const *)data->data, channels, data->frames, info->samples_per_sec,
			      data->timestamp, false);
}

void Provider::mix_audio(void *param, size_t, struct audio_data *data)
{
	auto *a = (AudioStream *)param;
	g_provider.push_audio(*a, (const float *const *)data->data, 2, data->frames, 48000, data->timestamp, true);
}

} // namespace

void start()
{
	g_provider.start();
}

void stop()
{
	g_provider.stop();
}

} // namespace provider
