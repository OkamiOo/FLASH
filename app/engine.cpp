#include "engine.h"

#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/security.h>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>

using json = nlohmann::json;
using namespace flash;

namespace {

std::wstring settings_path()
{
	PWSTR base = nullptr;
	std::wstring dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)))
		dir = std::wstring(base) + L"\\FLASH";
	CoTaskMemFree(base);
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir + L"\\reglages.json";
}

std::string computer_name()
{
	wchar_t name[256] = L"PC";
	DWORD size = 256;
	GetComputerNameW(name, &size);
	return to_utf8(name);
}

void make_inputs(const std::vector<SourceChoice> &sources, const EngineSettings &s,
		 std::vector<std::unique_ptr<VideoInput>> &video, std::vector<std::unique_ptr<AudioInput>> &audio)
{
	// La mire démarre sur une seconde pleine de l'horloge commune
	const uint64_t t0 = (now_ns() / 1'000'000'000ULL + 1) * 1'000'000'000ULL;
	for (const SourceChoice &c : sources) {
		if (c.kind == "mire") {
			video.push_back(make_mire_input(t0));
			audio.push_back(make_beep_input(t0));
		} else if (c.kind == "ecran") {
			video.push_back(make_screen_input(std::atoi(c.value.c_str()), s.max_w, s.max_h, s.cursor));
		} else if (c.kind == "fenetre") {
			video.push_back(make_window_input(c.value, s.max_w, s.max_h, s.cursor));
		} else if (c.kind == "appli") {
			audio.push_back(make_app_audio_input(c.value));
		} else if (c.kind == "systeme") {
			audio.push_back(make_system_audio_input());
		} else if (c.kind == "micro") {
			audio.push_back(make_mic_input(c.value));
		}
	}
}

} // namespace

Engine::Engine() = default;

Engine::~Engine()
{
	stop();
}

void Engine::load()
{
	std::ifstream f(settings_path());
	if (!f)
		return;
	const json j = json::parse(f, nullptr, false);
	if (!j.is_object())
		return;

	id_ = j.value("id", "");
	name_ = j.value("nom", "");
	code_ = j.value("code", "");
	if (j.contains("recepteurs") && j["recepteurs"].is_array()) {
		for (const auto &r : j["recepteurs"]) {
			const std::string key = unprotect_secret(r.value("cle", ""));
			if (!key.empty())
				receivers_.push_back({r.value("id", ""), r.value("nom", ""), key});
		}
	}
	if (!persist_)
		return; // en ligne de commande, sources et réglages viennent des arguments

	if (j.contains("sources") && j["sources"].is_array()) {
		for (const auto &s : j["sources"])
			sources_.push_back({s.value("type", ""), s.value("valeur", "")});
	}
	if (j.contains("reglages") && j["reglages"].is_object()) {
		const json &r = j["reglages"];
		settings_.address = r.value("adresse", settings_.address);
		settings_.port = (uint16_t)r.value("port", (int)settings_.port);
		settings_.latency_ms = r.value("latence", settings_.latency_ms);
		settings_.video_kbps = r.value("debit", settings_.video_kbps);
		settings_.hevc = r.value("hevc", settings_.hevc);
		settings_.encoder = r.value("encodeur", settings_.encoder);
		settings_.max_w = r.value("largeur", settings_.max_w);
		settings_.max_h = r.value("hauteur", settings_.max_h);
		settings_.cursor = r.value("curseur", settings_.cursor);
		settings_.open = r.value("ouvert", settings_.open);
	}
}

void Engine::save()
{
	json j;
	std::ifstream in(settings_path());
	if (in)
		j = json::parse(in, nullptr, false);
	in.close();
	if (!j.is_object())
		j = json::object();

	j["id"] = id_;
	j["nom"] = name_;
	j["code"] = code_;
	j["recepteurs"] = json::array();
	for (const Receiver &r : receivers_)
		j["recepteurs"].push_back({{"id", r.id}, {"nom", r.name}, {"cle", protect_secret(r.key)}});
	if (persist_) {
		j["sources"] = json::array();
		for (const SourceChoice &s : sources_)
			j["sources"].push_back({{"type", s.kind}, {"valeur", s.value}});
		j["reglages"] = {{"adresse", settings_.address}, {"port", settings_.port},     {"latence", settings_.latency_ms},
				 {"debit", settings_.video_kbps},  {"hevc", settings_.hevc},     {"encodeur", settings_.encoder},
				 {"largeur", settings_.max_w},     {"hauteur", settings_.max_h}, {"curseur", settings_.cursor},
				 {"ouvert", settings_.open}};
	}

	// Écriture dans un fichier à côté puis remplacement : jamais de réglages à moitié écrits
	const std::wstring path = settings_path();
	const std::wstring tmp = path + L".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << j.dump(2);
	}
	MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Engine::preset(const EngineSettings &s)
{
	std::lock_guard d(data_);
	settings_ = s;
}

bool Engine::start(bool persist)
{
	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		persist_ = persist;
		load();
		if (id_.empty())
			id_ = new_device_id();
		if (name_.empty())
			name_ = computer_name();
		if (code_.empty())
			code_ = new_invitation_code();
		save();
	}

	std::string gpu;
	device_ = create_d3d11_device(&gpu);
	if (!device_)
		return false;
	FLOG_I("GPU : %s", gpu.c_str());

	start_network();
	restart_sender();
	return true;
}

void Engine::start_network()
{
	std::string address, id, name;
	{
		std::lock_guard d(data_);
		address = settings_.address;
		id = id_;
		name = name_;
	}
	pairing_.start(address, kPairingPort, {id, name},
		       {[this] {
				std::lock_guard d(data_);
				return code_;
			},
			[this](const PeerInfo &r, const std::string &key) {
				std::lock_guard d(data_);
				receivers_.erase(std::remove_if(receivers_.begin(), receivers_.end(),
								[&](const Receiver &x) { return x.id == r.id; }),
						 receivers_.end());
				receivers_.push_back({r.id, r.name, key});
				save();
			}});
	discovery_.start(address, [this] { return announce(); });
}

void Engine::stop()
{
	std::lock_guard lock(mutex_);
	sender_.reset();
	discovery_.stop();
	pairing_.stop();
}

void Engine::restart_sender()
{
	if (sender_) {
		sender_->stop();
		sender_.reset();
	}
	std::vector<SourceChoice> sources;
	EngineSettings s;
	std::string name;
	{
		std::lock_guard d(data_);
		tracks_json_ = "[]";
		sources = sources_;
		s = settings_;
		name = name_;
	}
	if (sources.empty())
		return;

	std::vector<std::unique_ptr<VideoInput>> video;
	std::vector<std::unique_ptr<AudioInput>> audio;
	make_inputs(sources, s, video, audio);

	SenderOptions o;
	o.srt.address = s.address;
	o.srt.port = s.port;
	o.srt.latency_ms = s.latency_ms;
	o.srt.authorize = [this](const std::string &sid, std::string &pass) { return authorize(sid, pass); };
	o.video_kbps = s.video_kbps;
	o.codec = s.hevc ? Codec::HEVC : Codec::H264;
	o.encoder = s.encoder;
	o.host_name = name;

	auto sender = std::make_unique<Sender>();
	if (!sender->start(o, device_, std::move(video), std::move(audio)))
		return;
	std::lock_guard d(data_);
	tracks_json_ = sender->tracks_json();
	sender_ = std::move(sender);
}

// Un récepteur appairé reçoit sa clé ; un inconnu n'entre que si l'envoi est ouvert
bool Engine::authorize(const std::string &stream_id, std::string &passphrase)
{
	std::lock_guard d(data_);
	for (const Receiver &r : receivers_) {
		if (r.id == stream_id) {
			passphrase = r.key;
			return true;
		}
	}
	passphrase.clear();
	return settings_.open;
}

std::string Engine::announce()
{
	std::lock_guard d(data_);
	return make_announce(id_, name_, settings_.port, settings_.open, tracks_json_);
}

std::vector<SourceChoice> Engine::sources()
{
	std::lock_guard d(data_);
	return sources_;
}

void Engine::set_sources(const std::vector<SourceChoice> &sources)
{
	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		if (sources == sources_)
			return;
		sources_ = sources;
		save();
	}
	restart_sender();
}

EngineSettings Engine::settings()
{
	std::lock_guard d(data_);
	return settings_;
}

void Engine::set_settings(const EngineSettings &s)
{
	std::lock_guard lock(mutex_);
	bool rebind;
	{
		std::lock_guard d(data_);
		rebind = s.address != settings_.address;
		settings_ = s;
		save();
	}
	if (rebind) {
		discovery_.stop();
		pairing_.stop();
		start_network();
	}
	restart_sender();
}

std::string Engine::id()
{
	std::lock_guard d(data_);
	return id_;
}

std::string Engine::name()
{
	std::lock_guard d(data_);
	return name_;
}

std::string Engine::code()
{
	std::lock_guard d(data_);
	return code_;
}

void Engine::new_code()
{
	std::lock_guard d(data_);
	code_ = new_invitation_code();
	save();
}

std::vector<PeerInfo> Engine::receivers()
{
	std::lock_guard d(data_);
	std::vector<PeerInfo> out;
	for (const Receiver &r : receivers_)
		out.push_back({r.id, r.name});
	return out;
}

void Engine::forget_receiver(const std::string &id)
{
	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		receivers_.erase(
			std::remove_if(receivers_.begin(), receivers_.end(), [&](const Receiver &r) { return r.id == id; }),
			receivers_.end());
		save();
	}
	restart_sender(); // coupe sa connexion en cours
}

json Engine::status()
{
	std::lock_guard lock(mutex_);
	json j;
	{
		std::lock_guard d(data_);
		j = {{"id", id_}, {"nom", name_}, {"code", code_}};
		j["recepteurs"] = json::array();
		for (const Receiver &r : receivers_)
			j["recepteurs"].push_back({{"id", r.id}, {"nom", r.name}});
	}
	j["connectes"] = sender_ ? sender_->client_count() : 0;

	// Débits sur l'intervalle depuis le dernier appel
	const uint64_t now = now_ns();
	const double secs = last_stats_ns_ ? (now - last_stats_ns_) / 1e9 : 0;
	last_stats_ns_ = now;
	j["pistes"] = json::array();
	if (sender_) {
		for (const TrackStats &t : sender_->stats()) {
			j["pistes"].push_back({{"nom", t.name},
					       {"video", t.video},
					       {"active", t.active},
					       {"ips", t.video && secs > 0 ? t.frames / secs : 0},
					       {"kbps", secs > 0 ? t.bytes * 8 / secs / 1000 : 0},
					       {"encodage_ms", t.encode_ms_avg}});
		}
	}
	return j;
}
