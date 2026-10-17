#include "obs_client.h"

#include <flash/converter.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/obs_link.h>
#include <flash/preview.h>

#include <windows.h>
#include <dxgi.h>
#include <tlhelp32.h>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using json = nlohmann::json;
using namespace flash;
namespace ol = flash::obslink;

namespace {

// Ce que le plugin alimente : une entrée vidéo ou son ouverte par l'envoi
class Stream {
public:
	virtual ~Stream() = default;
	virtual std::vector<uint8_t> open_request(uint32_t id) = 0; // renvoyée à chaque connexion à OBS
	virtual void on_opened(const json &) {}
	virtual void on_frame(uint32_t, uint64_t) {}
	virtual void on_audio(uint64_t, uint32_t, const float *) {}
	virtual void on_closed() {}
};

// obs_ouvert : OBS tourne mais son plugin ne répond pas (plugin absent ou d'avant la 0.2.0)
json disconnected(bool obs_open = false)
{
	return {{"connecte", false}, {"obs_ouvert", obs_open}, {"scenes", json::array()}, {"sources", json::array()}};
}

bool obs_running()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return false;
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
		found = _wcsicmp(pe.szExeFile, L"obs64.exe") == 0;
	CloseHandle(snap);
	return found;
}

} // namespace

struct ObsClient::Impl {
	ComPtr<ID3D11Device> device;
	HANDLE stop_event = nullptr;
	std::thread thread;

	std::mutex mutex; // tout ce qui suit
	std::shared_ptr<ol::Link> link;
	std::map<uint32_t, Stream *> streams;
	std::set<uint32_t> closed; // fermées par OBS (scène supprimée, autre collection) : redemandées quand la liste change
	uint32_t next_id = 1;
	json catalog = disconnected();
	OnCatalog on_catalog;
	OnPreview on_preview;
	bool previews = false;
	std::mutex write_mutex;

	void run();
	void send(const std::vector<uint8_t> &bytes);
	void handle(const ol::Message &m);
	void publish(json c);
	uint32_t attach(Stream *s);
	void detach(uint32_t id);
};

void ObsClient::Impl::send(const std::vector<uint8_t> &bytes)
{
	std::shared_ptr<ol::Link> l;
	{
		std::lock_guard lock(mutex);
		l = link;
	}
	if (l) {
		std::lock_guard lock(write_mutex);
		l->write(bytes);
	}
}

void ObsClient::Impl::publish(json c)
{
	OnCatalog cb;
	{
		std::lock_guard lock(mutex);
		catalog = c;
		cb = on_catalog;
	}
	if (cb)
		cb(c);
}

uint32_t ObsClient::Impl::attach(Stream *s)
{
	uint32_t id;
	bool connected;
	{
		std::lock_guard lock(mutex);
		id = next_id++;
		streams[id] = s;
		connected = link != nullptr;
	}
	if (connected)
		send(s->open_request(id));
	return id;
}

void ObsClient::Impl::detach(uint32_t id)
{
	{
		std::lock_guard lock(mutex);
		streams.erase(id);
		closed.erase(id);
	}
	send(ol::pack(ol::Close, json({{"id", id}}).dump()));
}

// Connexion au plugin, refaite toutes les 2 s tant qu'OBS est fermé
void ObsClient::Impl::run()
{
	int announced = -1; // dernier état publié sans connexion : 0 OBS fermé, 1 OBS ouvert sans plugin
	while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
		HANDLE h = CreateFileW(ol::kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
				       nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			const int state = obs_running() ? 1 : 0;
			if (state != announced) {
				publish(disconnected(state == 1));
				announced = state;
			}
			WaitForSingleObject(stop_event, 2000);
			continue;
		}
		announced = -1;
		FLOG_I("OBS : connecté au plugin FLASH");
		auto l = std::make_shared<ol::Link>(h, stop_event);
		std::vector<std::vector<uint8_t>> opens;
		bool want_previews;
		{
			std::lock_guard lock(mutex);
			link = l;
			closed.clear();
			for (auto &[id, s] : streams)
				opens.push_back(s->open_request(id));
			want_previews = previews;
		}
		send(ol::pack(ol::Previews, json({{"actif", want_previews}}).dump()));
		for (const auto &o : opens)
			send(o);

		ol::Message m;
		while (l->read(m))
			handle(m);

		{
			std::lock_guard lock(mutex);
			link.reset();
			for (auto &[id, s] : streams)
				s->on_closed();
		}
		CloseHandle(h);
		FLOG_I("OBS : déconnecté du plugin FLASH");
		publish(disconnected());
	}
}

void ObsClient::Impl::handle(const ol::Message &m)
{
	auto u32 = [&](size_t at) {
		uint32_t v = 0;
		if (at + 4 <= m.body.size())
			std::memcpy(&v, m.body.data() + at, 4);
		return v;
	};
	auto u64 = [&](size_t at) {
		uint64_t v = 0;
		if (at + 8 <= m.body.size())
			std::memcpy(&v, m.body.data() + at, 8);
		return v;
	};

	switch (m.type) {
	case ol::Catalog: {
		json c = json::parse(m.body.begin(), m.body.end(), nullptr, false);
		if (!c.is_object())
			return;
		c["connecte"] = true;
		publish(c);
		// Liste changée : la scène ou la source d'une entrée fermée est peut-être revenue
		std::vector<std::vector<uint8_t>> opens;
		{
			std::lock_guard lock(mutex);
			for (uint32_t id : closed)
				if (auto it = streams.find(id); it != streams.end())
					opens.push_back(it->second->open_request(id));
			closed.clear();
		}
		for (const auto &o : opens)
			send(o);
		break;
	}
	case ol::Opened:
	case ol::Closed: {
		const json j = json::parse(m.body.begin(), m.body.end(), nullptr, false);
		if (!j.is_object())
			return;
		std::lock_guard lock(mutex);
		const uint32_t id = j.value("id", 0u);
		auto it = streams.find(id);
		if (it == streams.end())
			return;
		if (m.type == ol::Opened) {
			closed.erase(id);
			it->second->on_opened(j);
		} else {
			closed.insert(id);
			it->second->on_closed();
		}
		break;
	}
	case ol::Frame: {
		std::lock_guard lock(mutex);
		auto it = streams.find(u32(0));
		if (it != streams.end())
			it->second->on_frame(u32(4), u64(8));
		break;
	}
	case ol::Audio: {
		const uint32_t frames = u32(12);
		if (16 + (size_t)frames * 8 > m.body.size())
			return;
		std::lock_guard lock(mutex);
		auto it = streams.find(u32(0));
		if (it != streams.end())
			it->second->on_audio(u64(4), frames, (const float *)(m.body.data() + 16));
		break;
	}
	case ol::Preview: {
		if (m.body.size() < 6)
			return;
		uint16_t len = 0, w = 0, h = 0;
		std::memcpy(&len, m.body.data(), 2);
		if (6 + (size_t)len > m.body.size())
			return;
		const std::string name((const char *)m.body.data() + 2, len);
		std::memcpy(&w, m.body.data() + 2 + len, 2);
		std::memcpy(&h, m.body.data() + 4 + len, 2);
		if (6 + len + (size_t)w * h * 4 > m.body.size())
			return;
		std::vector<uint8_t> jpeg;
		if (!encode_jpeg(m.body.data() + 6 + len, w, h, (uint32_t)w * 4, jpeg))
			return;
		OnPreview cb;
		bool scene = false;
		{
			std::lock_guard lock(mutex);
			cb = on_preview;
			for (const auto &s : catalog.value("scenes", json::array()))
				scene = scene || s.value("nom", "") == name;
		}
		if (cb)
			cb((name.empty() ? "obs-programme|" : scene ? "obs-scene|" : "obs-source|") + name, jpeg);
		break;
	}
	default:
		break;
	}
}

namespace {

// Une scène, une source ou le programme d'OBS : OBS rend dans trois textures partagées, l'appli
// les convertit en NV12 sur le GPU, comme une capture d'écran
class ObsVideo final : public VideoInput, public Stream {
public:
	ObsVideo(std::shared_ptr<ObsClient::Impl> client, std::string kind, std::string name, uint32_t w, uint32_t h)
		: client_(std::move(client)), kind_(std::move(kind)), name_(std::move(name)), w_(w), h_(h)
	{
	}
	~ObsVideo() override { stop(); }

	bool start(ID3D11Device *device, OnFrame on_frame) override
	{
		device_ = device;
		if (!converter_.init(device, w_, h_))
			return false;
		on_frame_ = std::move(on_frame);
		id_ = client_->attach(this);
		return true;
	}

	void stop() override
	{
		if (id_) {
			client_->detach(id_);
			id_ = 0;
		}
		std::lock_guard lock(mutex_);
		slots_.clear();
	}

	uint32_t width() const override { return w_; }
	uint32_t height() const override { return h_; }
	std::string name() const override { return kind_ == "obs-programme" ? "Programme OBS" : name_; }

	std::vector<uint8_t> open_request(uint32_t id) override
	{
		const std::string type = kind_ == "obs-scene" ? "scene" : kind_ == "obs-programme" ? "programme" : "source";
		return ol::pack(ol::Open, json({{"id", id}, {"type", type}, {"nom", name_}, {"largeur", w_}, {"hauteur", h_}}).dump());
	}

	void on_opened(const json &j) override
	{
		std::lock_guard lock(mutex_);
		slots_.clear();
		for (const auto &h : j.value("poignees", json::array())) {
			Slot s;
			if (FAILED(device_->OpenSharedResource((HANDLE)(uintptr_t)h.get<uint32_t>(), IID_PPV_ARGS(&s.tex))) ||
			    FAILED(s.tex.As(&s.lock))) {
				FLOG_E("OBS : texture partagée de « %s » impossible à ouvrir", name().c_str());
				slots_.clear();
				return;
			}
			slots_.push_back(s);
		}
	}

	void on_frame(uint32_t index, uint64_t time_ns) override
	{
		std::lock_guard lock(mutex_);
		if (index >= slots_.size())
			return;
		Slot &s = slots_[index];
		if (s.lock->AcquireSync(1, 100) != S_OK)
			return;
		ID3D11Texture2D *nv12 = converter_.convert(s.tex.Get());
		s.lock->ReleaseSync(0); // OBS peut réutiliser la texture une fois la conversion faite par le GPU
		if (nv12 && on_frame_)
			on_frame_(nv12, time_ns);
	}

	void on_closed() override
	{
		std::lock_guard lock(mutex_);
		slots_.clear();
	}

private:
	struct Slot {
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<IDXGIKeyedMutex> lock;
	};

	std::shared_ptr<ObsClient::Impl> client_;
	const std::string kind_, name_;
	const uint32_t w_, h_;
	ComPtr<ID3D11Device> device_;
	Nv12Converter converter_;
	OnFrame on_frame_;
	uint32_t id_ = 0;
	std::mutex mutex_;
	std::vector<Slot> slots_;
};

// Le son d'une source d'OBS, ou son mix (nom vide), déjà en 48 kHz stéréo
class ObsAudio final : public AudioInput, public Stream {
public:
	ObsAudio(std::shared_ptr<ObsClient::Impl> client, std::string name) : client_(std::move(client)), name_(std::move(name)) {}
	~ObsAudio() override { stop(); }

	bool start(OnSamples on_samples) override
	{
		on_samples_ = std::move(on_samples);
		id_ = client_->attach(this);
		return true;
	}

	void stop() override
	{
		if (id_) {
			client_->detach(id_);
			id_ = 0;
		}
	}

	std::string name() const override { return name_.empty() ? "Mix OBS" : name_; }

	std::vector<uint8_t> open_request(uint32_t id) override
	{
		return ol::pack(ol::OpenAudio, json({{"id", id}, {"nom", name_}}).dump());
	}

	void on_audio(uint64_t time_ns, uint32_t frames, const float *samples) override
	{
		if (on_samples_)
			on_samples_(samples, frames, time_ns);
	}

private:
	std::shared_ptr<ObsClient::Impl> client_;
	const std::string name_;
	OnSamples on_samples_;
	uint32_t id_ = 0;
};

} // namespace

ObsClient::ObsClient(ComPtr<ID3D11Device> device) : impl_(std::make_shared<Impl>())
{
	impl_->device = device;
	impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	impl_->thread = std::thread([impl = impl_.get()] {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		impl->run();
		CoUninitialize();
	});
}

ObsClient::~ObsClient()
{
	SetEvent(impl_->stop_event);
	impl_->thread.join();
	CloseHandle(impl_->stop_event);
}

void ObsClient::set_listener(OnCatalog on_catalog, OnPreview on_preview)
{
	std::lock_guard lock(impl_->mutex);
	impl_->on_catalog = std::move(on_catalog);
	impl_->on_preview = std::move(on_preview);
}

json ObsClient::catalog()
{
	std::lock_guard lock(impl_->mutex);
	return impl_->catalog;
}

void ObsClient::set_previews(bool on)
{
	{
		std::lock_guard lock(impl_->mutex);
		if (impl_->previews == on)
			return;
		impl_->previews = on;
	}
	impl_->send(ol::pack(ol::Previews, json({{"actif", on}}).dump()));
}

std::unique_ptr<VideoInput> ObsClient::video_input(const std::string &kind, const std::string &name, uint32_t max_w,
						   uint32_t max_h)
{
	// Taille d'envoi : celle du canevas d'OBS pour une scène ou le programme, celle de la source
	// sinon, ramenée dans la taille maximale
	uint32_t sw = 1920, sh = 1080;
	{
		std::lock_guard lock(impl_->mutex);
		const json &c = impl_->catalog;
		if (c.contains("base") && c["base"].is_array() && c["base"].size() == 2) {
			sw = c["base"][0].get<uint32_t>();
			sh = c["base"][1].get<uint32_t>();
		}
		if (kind == "obs-source") {
			for (const auto &s : c.value("sources", json::array())) {
				if (s.value("nom", "") == name && s.value("largeur", 0u) > 0) {
					sw = s.value("largeur", sw);
					sh = s.value("hauteur", sh);
				}
			}
		}
	}
	uint32_t w, h;
	fit_size(std::max<uint32_t>(sw, 16), std::max<uint32_t>(sh, 16), max_w, max_h, w, h);
	return std::make_unique<ObsVideo>(impl_, kind, name, w, h);
}

std::unique_ptr<AudioInput> ObsClient::audio_input(const std::string &name)
{
	return std::make_unique<ObsAudio>(impl_, name);
}
