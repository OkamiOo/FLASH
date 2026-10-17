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

constexpr uint64_t kDiscoveryFreshNs = 3'000'000'000ULL;

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

// Plusieurs FLASH peuvent tourner à la fois (interface, ligne de commande) : chacun relit le
// fichier puis le remplace sous ce verrou commun, sinon l'un efface ce que l'autre vient d'écrire.
class SettingsLock {
public:
	SettingsLock() : mutex_(CreateMutexW(nullptr, FALSE, L"Local\\FLASH-reglages"))
	{
		if (mutex_)
			WaitForSingleObject(mutex_, 5000);
	}
	~SettingsLock()
	{
		if (mutex_) {
			ReleaseMutex(mutex_);
			CloseHandle(mutex_);
		}
	}

private:
	HANDLE mutex_;
};

// Contenu du fichier ; faux s'il existe mais reste illisible (en cours de remplacement…)
bool read_settings(json &j)
{
	const std::wstring path = settings_path();
	for (int attempt = 0; attempt < 5; attempt++) {
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			j = json::object();
			return true;
		}
		std::ifstream in(path);
		if (in) {
			j = json::parse(in, nullptr, false);
			if (j.is_object())
				return true;
		}
		Sleep(20);
	}
	j = json::object();
	return false;
}

std::string computer_name()
{
	wchar_t name[256] = L"PC";
	DWORD size = 256;
	GetComputerNameW(name, &size);
	return to_utf8(name);
}

bool is_video_kind(const std::string &kind)
{
	return kind == "ecran" || kind == "fenetre" || kind == "obs-scene" || kind == "obs-source" || kind == "obs-programme";
}

// Ajoute à l'envoi la ou les pistes d'une source (la mire en donne deux : image et bip)
void add_to_sender(Sender &sender, const SourceChoice &c, const EngineSettings &s, ObsClient *obs)
{
	const std::string key = c.key();
	if (c.kind == "mire") {
		// La mire démarre sur une seconde pleine de l'horloge commune
		const uint64_t t0 = (now_ns() / 1'000'000'000ULL + 1) * 1'000'000'000ULL;
		sender.add_video(key + "image", make_mire_input(t0));
		sender.add_audio(key + "son", make_beep_input(t0), c.volume);
	} else if (c.kind == "ecran") {
		sender.add_video(key, make_screen_input(std::atoi(c.value.c_str()), s.max_w, s.max_h, s.cursor));
	} else if (c.kind == "fenetre") {
		sender.add_video(key, make_window_input(c.value, s.max_w, s.max_h, s.cursor));
	} else if (c.kind == "appli") {
		sender.add_audio(key, make_app_audio_input(c.value), c.volume);
	} else if (c.kind == "systeme") {
		sender.add_audio(key, make_system_audio_input(), c.volume);
	} else if (c.kind == "micro") {
		sender.add_audio(key, make_mic_input(c.value), c.volume);
	} else if (obs && (c.kind == "obs-scene" || c.kind == "obs-source" || c.kind == "obs-programme")) {
		sender.add_video(key, obs->video_input(c.kind, c.value, s.max_w, s.max_h));
	} else if (obs && c.kind == "obs-son") {
		sender.add_audio(key, obs->audio_input(c.value), c.volume);
	}
}

void remove_from_sender(Sender &sender, const SourceChoice &c)
{
	const std::string key = c.key();
	if (c.kind == "mire") {
		sender.remove(key + "image");
		sender.remove(key + "son");
	} else {
		sender.remove(key);
	}
}

VideoOptions video_options(const EngineSettings &s)
{
	VideoOptions v;
	v.kbps = s.video_kbps;
	v.codec = s.hevc ? Codec::HEVC : Codec::H264;
	v.encoder = s.encoder;
	return v;
}

} // namespace

Engine::Engine() = default;

Engine::~Engine()
{
	stop();
}

void Engine::load()
{
	json j;
	{
		SettingsLock lock;
		read_settings(j);
	}
	if (j.empty())
		return;

	id_ = j.value("id", "");
	name_ = j.value("nom", "");
	code_ = j.value("code", "");
	auto peers = [&](const char *key, std::vector<Peer> &out) {
		if (!j.contains(key) || !j[key].is_array())
			return;
		for (const auto &r : j[key]) {
			const std::string secret = unprotect_secret(r.value("cle", ""));
			if (!secret.empty())
				out.push_back({r.value("id", ""), r.value("nom", ""), secret, r.value("adresse", "")});
		}
	};
	peers("recepteurs", receivers_);
	peers("emetteurs", senders_);
	if (!persist_)
		return; // en ligne de commande, sources et réglages viennent des arguments

	if (j.contains("volumes") && j["volumes"].is_object()) {
		for (const auto &[key, v] : j["volumes"].items())
			if (v.is_number())
				volumes_[key] = v.get<float>();
	}
	if (j.contains("fenetres") && j["fenetres"].is_array()) {
		for (const auto &f : j["fenetres"])
			if (f.is_string())
				windows_.push_back(f.get<std::string>());
	}
	if (j.contains("sources") && j["sources"].is_array()) {
		for (const auto &s : j["sources"]) {
			SourceChoice c{s.value("type", ""), s.value("valeur", "")};
			if (auto it = volumes_.find(c.key()); it != volumes_.end())
				c.volume = it->second;
			sources_.push_back(c);
			if (c.kind == "fenetre" && std::find(windows_.begin(), windows_.end(), c.value) == windows_.end())
				windows_.push_back(c.value);
		}
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

void Engine::save(unsigned parts)
{
	// Relu à chaque fois : une autre instance (ligne de commande) a pu écrire ses propres parties.
	// Illisible : tout ce qu'on sait est réécrit, plutôt que de perdre le reste.
	SettingsLock lock;
	json j;
	if (!read_settings(j))
		parts = kIdentity | kReceivers | kSenders | kSettings;

	auto peers = [](const std::vector<Peer> &list, bool address) {
		json out = json::array();
		for (const Peer &p : list) {
			json e = {{"id", p.id}, {"nom", p.name}, {"cle", protect_secret(p.key)}};
			if (address)
				e["adresse"] = p.address;
			out.push_back(e);
		}
		return out;
	};
	if (parts & kIdentity) {
		j["id"] = id_;
		j["nom"] = name_;
		j["code"] = code_;
	}
	if (parts & kReceivers)
		j["recepteurs"] = peers(receivers_, false);
	if (parts & kSenders)
		j["emetteurs"] = peers(senders_, true);
	if ((parts & kSettings) && persist_) {
		j["sources"] = json::array();
		for (const SourceChoice &s : sources_)
			j["sources"].push_back({{"type", s.kind}, {"valeur", s.value}});
		j["volumes"] = json::object();
		for (const auto &[key, v] : volumes_)
			j["volumes"][key] = v;
		j["fenetres"] = windows_;
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
	for (int attempt = 0; attempt < 5 && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING); attempt++)
		Sleep(20);
}

void Engine::preset(const EngineSettings &s)
{
	std::lock_guard d(data_);
	settings_ = s;
}

bool Engine::start(bool persist, bool network)
{
	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		persist_ = persist;
		network_ = network;
		load();
		if (id_.empty())
			id_ = new_device_id();
		if (name_.empty())
			name_ = computer_name();
		if (code_.empty())
			code_ = new_invitation_code();
		self_key_ = new_device_id() + new_device_id();
		save(kIdentity | kSettings);
	}
	if (!network)
		return true;

	std::string gpu;
	device_ = create_d3d11_device(&gpu);
	if (!device_)
		return false;
	FLOG_I("GPU : %s", gpu.c_str());

	start_network();
	obs_ = std::make_unique<ObsClient>(device_);
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
								[&](const Peer &x) { return x.id == r.id; }),
						 receivers_.end());
				receivers_.push_back({r.id, r.name, key, ""});
				save(kReceivers);
			}});
	discovery_.start(address, [this] { return announce(); });
}

void Engine::stop()
{
	std::map<std::string, std::unique_ptr<Viewer>> views;
	{
		std::lock_guard v(views_mutex_);
		views.swap(views_);
	}
	views.clear();

	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		if (volumes_dirty_)
			save(kSettings);
		volumes_dirty_ = false;
	}
	sender_.reset(); // les entrées OBS se détachent du client avant qu'il s'arrête
	obs_.reset();
	discovery_.stop();
	pairing_.stop();
}

// L'envoi écoute dès le démarrage, même sans source : un récepteur reste connecté pendant
// qu'on choisit ce qu'on envoie. Refait seulement quand l'adresse, le port ou la latence changent.
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
	if (!device_)
		return;

	SenderOptions o;
	o.srt.address = s.address;
	o.srt.port = s.port;
	o.srt.latency_ms = s.latency_ms;
	o.srt.authorize = [this](const std::string &sid, std::string &pass) { return authorize(sid, pass); };
	o.video = video_options(s);
	o.host_name = name;

	auto sender = std::make_unique<Sender>();
	if (!sender->start(o, device_))
		return;
	for (const SourceChoice &c : sources)
		add_to_sender(*sender, c, s, obs_.get());
	std::lock_guard d(data_);
	tracks_json_ = sender->tracks_json();
	sender_ = std::move(sender);
}

void Engine::tracks_changed()
{
	if (!sender_)
		return;
	const std::string tracks = sender_->tracks_json();
	std::lock_guard d(data_);
	tracks_json_ = tracks;
}

// Un récepteur appairé reçoit sa clé, la visionneuse de ce PC la clé de la session ; un inconnu
// n'entre que si l'envoi est ouvert
bool Engine::authorize(const std::string &stream_id, std::string &passphrase)
{
	std::lock_guard d(data_);
	for (const Peer &r : receivers_) {
		if (r.id == stream_id) {
			passphrase = r.key;
			return true;
		}
	}
	if (stream_id == id_ && !self_key_.empty()) {
		passphrase = self_key_;
		return true;
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

// Seules les différences sont appliquées : une source retirée coupe sa piste, une source
// ajoutée en crée une, les autres continuent sans interruption
void Engine::set_sources(const std::vector<SourceChoice> &sources)
{
	std::lock_guard lock(mutex_);
	std::vector<SourceChoice> removed, added, volume;
	EngineSettings s;
	{
		std::lock_guard d(data_);
		for (const SourceChoice &old : sources_)
			if (std::find(sources.begin(), sources.end(), old) == sources.end())
				removed.push_back(old);
		for (const SourceChoice &c : sources) {
			auto it = std::find(sources_.begin(), sources_.end(), c);
			if (it == sources_.end())
				added.push_back(c);
			else if (it->volume != c.volume)
				volume.push_back(c);
		}
		if (removed.empty() && added.empty() && volume.empty())
			return;
		sources_ = sources;
		s = settings_;
		save(kSettings);
	}
	if (!sender_)
		return;
	for (const SourceChoice &c : removed)
		remove_from_sender(*sender_, c);
	for (const SourceChoice &c : added)
		add_to_sender(*sender_, c, s, obs_.get());
	for (const SourceChoice &c : volume)
		sender_->set_gain(c.kind == "mire" ? c.key() + "son" : c.key(), c.volume);
	tracks_changed();
}

void Engine::set_source(const SourceChoice &source, bool on)
{
	std::vector<SourceChoice> list = sources();
	auto it = std::find(list.begin(), list.end(), source);
	if (on && it == list.end()) {
		SourceChoice c = source;
		c.volume = volume_of(source);
		list.push_back(c);
	}
	else if (!on && it != list.end())
		list.erase(it);
	set_sources(list);
}

// Volume envoyé : gardé avec la source, appliqué tout de suite à sa piste si elle est envoyée
void Engine::set_volume(const SourceChoice &source, float volume)
{
	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		auto it = std::find(sources_.begin(), sources_.end(), source);
		if (it != sources_.end())
			it->volume = volume;
		volumes_[source.key()] = volume;
		// Un curseur qu'on fait glisser envoie des dizaines de valeurs : le disque n'en voit
		// qu'une toutes les demi-secondes, la dernière est écrite à l'arrêt
		const uint64_t now = now_ns();
		volumes_dirty_ = true;
		if (now - volumes_saved_ > 500'000'000ULL) {
			save(kSettings);
			volumes_saved_ = now;
			volumes_dirty_ = false;
		}
	}
	if (sender_)
		sender_->set_gain(source.kind == "mire" ? source.key() + "son" : source.key(), volume);
}

float Engine::volume_of(const SourceChoice &source)
{
	std::lock_guard d(data_);
	auto it = volumes_.find(source.key());
	return it != volumes_.end() ? it->second : 1.0f;
}

std::map<std::string, float> Engine::volumes()
{
	std::lock_guard d(data_);
	return volumes_;
}

std::vector<std::string> Engine::windows()
{
	std::lock_guard d(data_);
	return windows_;
}

void Engine::add_window(const std::string &exe)
{
	{
		std::lock_guard d(data_);
		if (std::find(windows_.begin(), windows_.end(), exe) == windows_.end())
			windows_.push_back(exe);
		save(kSettings);
	}
	set_source({"fenetre", exe}, true);
}

void Engine::remove_window(const std::string &exe)
{
	set_source({"fenetre", exe}, false);
	std::lock_guard d(data_);
	windows_.erase(std::remove(windows_.begin(), windows_.end(), exe), windows_.end());
	save(kSettings);
}

EngineSettings Engine::settings()
{
	std::lock_guard d(data_);
	return settings_;
}

// Adresse, port ou latence : envoi refait. Débit, format, encodeur : encodeurs vidéo refaits,
// captures et pistes son intactes. Taille ou curseur : captures vidéo refaites.
void Engine::set_settings(const EngineSettings &s)
{
	std::lock_guard lock(mutex_);
	EngineSettings old;
	std::vector<SourceChoice> sources;
	{
		std::lock_guard d(data_);
		old = settings_;
		settings_ = s;
		sources = sources_;
		save(kSettings);
	}
	const bool rebind = s.address != old.address;
	if (rebind && network_) {
		discovery_.stop();
		pairing_.stop();
		start_network();
	}
	if (rebind || s.port != old.port || s.latency_ms != old.latency_ms || !sender_) {
		restart_sender();
		return;
	}
	if (s.video_kbps != old.video_kbps || s.hevc != old.hevc || s.encoder != old.encoder)
		sender_->set_video_options(video_options(s));
	if (s.max_w != old.max_w || s.max_h != old.max_h || s.cursor != old.cursor) {
		for (const SourceChoice &c : sources) {
			if (is_video_kind(c.kind)) {
				remove_from_sender(*sender_, c);
				add_to_sender(*sender_, c, s, obs_.get());
			}
		}
	}
	tracks_changed();
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
	save(kIdentity);
}

std::vector<PeerInfo> Engine::receivers()
{
	std::lock_guard d(data_);
	std::vector<PeerInfo> out;
	for (const Peer &r : receivers_)
		out.push_back({r.id, r.name});
	return out;
}

void Engine::forget_receiver(const std::string &id)
{
	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		receivers_.erase(
			std::remove_if(receivers_.begin(), receivers_.end(), [&](const Peer &r) { return r.id == id; }),
			receivers_.end());
		save(kReceivers);
	}
	restart_sender(); // coupe sa connexion en cours
}

// ---------- Réception ----------

std::vector<SenderAnnounce> Engine::discover(bool refresh)
{
	std::lock_guard lock(discover_mutex_);
	if (refresh || !seen_at_ || now_ns() - seen_at_ > kDiscoveryFreshNs) {
		seen_ = discover_senders(500);
		seen_at_ = now_ns();
	}
	return seen_;
}

json Engine::senders_json(bool refresh)
{
	const std::vector<SenderAnnounce> found = discover(refresh);
	std::vector<Peer> paired;
	std::string self, self_name;
	{
		std::lock_guard d(data_);
		paired = senders_;
		self = id_;
		self_name = name_;
	}
	std::vector<std::string> open_views;
	{
		std::lock_guard v(views_mutex_);
		for (const auto &[key, view] : views_)
			open_views.push_back(key);
	}
	auto is_open = [&](const std::string &id) {
		return std::find(open_views.begin(), open_views.end(), id) != open_views.end();
	};

	json list = json::array();
	for (const SenderAnnounce &s : found) {
		const bool me = s.id == self;
		if (me && s.tracks.empty())
			continue; // ce PC n'envoie rien : rien à voir
		const bool is_paired =
			std::any_of(paired.begin(), paired.end(), [&](const Peer &p) { return p.id == s.id; });
		json tracks = json::array();
		for (const AnnouncedTrack &t : s.tracks)
			tracks.push_back({{"nom", t.name}, {"video", t.video}});
		list.push_back({{"id", s.id},
				{"nom", me ? self_name : s.name},
				{"adresse", s.address},
				{"port_appairage", s.pairing_port},
				{"trouve", true},
				{"appaire", is_paired},
				{"ouvert", s.open},
				{"moi", me},
				{"vue", is_open(s.id)},
				{"pistes", tracks}});
	}
	for (const Peer &p : paired) {
		if (std::any_of(found.begin(), found.end(), [&](const SenderAnnounce &s) { return s.id == p.id; }))
			continue;
		list.push_back({{"id", p.id},
				{"nom", p.name},
				{"adresse", p.address},
				{"trouve", false},
				{"appaire", true},
				{"ouvert", false},
				{"moi", false},
				{"vue", is_open(p.id)},
				{"pistes", json::array()}});
	}
	return {{"type", "emetteurs"}, {"liste", list}, {"sorties", list_audio_outputs()}};
}

PairingResult Engine::pair(const std::string &address, uint16_t port, const std::string &code)
{
	const PairingResult r = pair_with_sender(address, port ? port : kPairingPort, code, {id(), name()});
	if (!r.ok)
		return r;
	std::lock_guard d(data_);
	senders_.erase(std::remove_if(senders_.begin(), senders_.end(), [&](const Peer &p) { return p.id == r.sender.id; }),
		       senders_.end());
	senders_.push_back({r.sender.id, r.sender.name, r.key_hex, address});
	save(kSenders);
	return r;
}

void Engine::forget_sender(const std::string &id)
{
	close_view(id);
	std::lock_guard d(data_);
	senders_.erase(std::remove_if(senders_.begin(), senders_.end(), [&](const Peer &p) { return p.id == id; }),
		       senders_.end());
	save(kSenders);
}

// Avant chaque tentative de connexion d'une visionneuse : où est l'émetteur, et avec quelle clé
bool Engine::refresh_target(const std::string &sender, SrtOptions &o, int &failures)
{
	std::string key, stored;
	bool self;
	{
		std::lock_guard d(data_);
		o.stream_id = id_;
		self = sender == id_;
		for (const Peer &p : senders_) {
			if (p.id == sender) {
				key = p.key;
				stored = p.address;
			}
		}
		if (self && key.empty()) {
			// Aperçu de ce PC : en local, avec la clé de la session
			o.address = "127.0.0.1";
			o.port = settings_.port;
			o.passphrase = self_key_;
			return true;
		}
	}

	for (const SenderAnnounce &s : discover(failures > 0)) {
		if (s.id == sender) {
			o.address = s.address;
			o.port = s.srt_port;
			o.passphrase = key;
			failures = 0;
			return true;
		}
	}
	if (failures++ == 0)
		FLOG_I("Émetteur %s introuvable sur le réseau pour l'instant", sender.c_str());
	if (stored.empty())
		return false;
	o.address = stored; // adresse de l'appairage, si la découverte ne passe pas (autre réseau)
	o.passphrase = key;
	return true;
}

bool Engine::watch(const WatchRequest &r, std::string &error)
{
	const std::string self = id();
	const std::string sid = r.sender == "moi" ? self : r.sender;
	const std::string key = sid.empty() ? r.address : sid;
	if (key.empty()) {
		error = "Aucun émetteur choisi";
		return false;
	}

	ViewerOptions v;
	const EngineSettings s = settings();
	v.srt.latency_ms = s.latency_ms;
	v.srt.stream_id = self;
	std::string title;
	if (!sid.empty()) {
		{
			std::lock_guard d(data_);
			if (sid == id_)
				title = name_ + " (aperçu)";
			for (const Peer &p : senders_) {
				if (p.id == sid)
					title = p.name;
			}
		}
		if (title.empty()) {
			for (const SenderAnnounce &a : discover(false)) {
				if (a.id == sid)
					title = a.name;
			}
		}
		auto failures = std::make_shared<int>(0);
		v.srt.refresh = [this, sid, failures](SrtOptions &o) { return refresh_target(sid, o, *failures); };
	} else {
		v.srt.address = r.address;
		title = r.address;
	}
	if (title.empty())
		title = "Émetteur";
	v.title = title + (r.video.empty() || r.video == "-" ? "" : " : " + r.video);
	v.video = r.video;
	v.audio = r.audio;
	v.output = r.output;
	v.fullscreen = r.fullscreen;
	v.screen = r.screen;
	v.background = r.background;

	// Une seule visionneuse par émetteur : l'ancienne se ferme d'abord
	close_view(key);
	auto view = std::make_unique<Viewer>(v);
	std::lock_guard lock(views_mutex_);
	views_[key] = std::move(view);
	return true;
}

void Engine::close_view(const std::string &sender)
{
	std::unique_ptr<Viewer> old;
	{
		std::lock_guard lock(views_mutex_);
		auto it = views_.find(sender);
		if (it == views_.end())
			return;
		old = std::move(it->second);
		views_.erase(it);
	}
}

// Les fenêtres fermées par l'utilisateur sont retirées ici
json Engine::views_json()
{
	std::vector<std::unique_ptr<Viewer>> closed;
	json list = json::array();
	{
		std::lock_guard lock(views_mutex_);
		for (auto it = views_.begin(); it != views_.end();) {
			if (it->second->closed()) {
				closed.push_back(std::move(it->second));
				it = views_.erase(it);
				continue;
			}
			json st = it->second->status();
			st["id"] = it->first;
			list.push_back(st);
			++it;
		}
	}
	return list;
}

size_t Engine::views()
{
	std::vector<std::unique_ptr<Viewer>> closed;
	std::lock_guard lock(views_mutex_);
	for (auto it = views_.begin(); it != views_.end();) {
		if (it->second->closed()) {
			closed.push_back(std::move(it->second));
			it = views_.erase(it);
		} else {
			++it;
		}
	}
	return views_.size();
}

json Engine::status()
{
	json j;
	j["vues"] = views_json();
	bool self_view = false;
	for (const auto &v : j["vues"])
		self_view = self_view || v.value("id", "") == id();

	std::lock_guard lock(mutex_);
	{
		std::lock_guard d(data_);
		j["id"] = id_;
		j["nom"] = name_;
		j["code"] = code_;
		j["recepteurs"] = json::array();
		for (const Peer &r : receivers_)
			j["recepteurs"].push_back({{"id", r.id}, {"nom", r.name}});
	}
	// L'aperçu de ce PC ne compte pas comme un PC qui reçoit
	size_t clients = sender_ ? sender_->client_count() : 0;
	if (self_view && clients > 0)
		clients--;
	j["connectes"] = clients;

	// Débits sur l'intervalle depuis le dernier appel
	const uint64_t now = now_ns();
	const double secs = last_stats_ns_ ? (now - last_stats_ns_) / 1e9 : 0;
	last_stats_ns_ = now;
	j["pistes"] = json::array();
	double kbps_total = 0;
	if (sender_) {
		for (const TrackStats &t : sender_->stats()) {
			const double kbps = secs > 0 ? t.bytes * 8 / secs / 1000 : 0;
			kbps_total += kbps;
			j["pistes"].push_back({{"cle", t.key},
					       {"nom", t.name},
					       {"video", t.video},
					       {"active", t.active},
					       {"ips", t.video && secs > 0 ? t.frames / secs : 0},
					       {"kbps", kbps},
					       {"encodage_ms", t.encode_ms_avg}});
		}
	}
	// En-tête : débit envoyé et latence de bout en bout du récepteur le plus lent
	j["debit_kbps"] = kbps_total;
	j["latence_ms"] = sender_ ? sender_->receiver_latency_ms() : 0;
	return j;
}

std::map<std::string, float> Engine::levels()
{
	std::lock_guard lock(mutex_);
	return sender_ ? sender_->levels() : std::map<std::string, float>{};
}

ComPtr<ID3D11Texture2D> Engine::last_frame(const std::string &key)
{
	std::lock_guard lock(mutex_);
	return sender_ ? sender_->last_frame(key) : nullptr;
}
