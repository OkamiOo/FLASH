#pragma once

#include "obs_client.h"
#include "viewer.h"

#include <flash/discovery.h>
#include <flash/pairing.h>
#include <flash/sender.h>

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Le cœur de l'appli : réglages, sources choisies, envoi, appairage, découverte et
// visionneuses. Les réglages vivent dans %APPDATA%\FLASH\reglages.json ; les clés des
// récepteurs et des émetteurs appairés y sont chiffrées pour l'utilisateur Windows (DPAPI).

struct SourceChoice {
	std::string kind; // mire, ecran, fenetre, appli, systeme, micro, obs-scene, obs-source, obs-programme, obs-son
	std::string value;
	float volume = 1.0f; // son : volume envoyé (0 à 1)

	std::string key() const { return kind + "|" + value; }
	bool operator==(const SourceChoice &o) const { return kind == o.kind && value == o.value; }
};

struct EngineSettings {
	std::string address = "0.0.0.0"; // 127.0.0.1 : ce PC seulement
	uint16_t port = 9710;
	int latency_ms = 20;
	uint32_t video_kbps = 25000;
	bool hevc = false;
	std::string encoder = "auto";
	uint32_t max_w = 1920, max_h = 1080;
	bool cursor = true;
	bool open = false; // accepter les récepteurs sans appairage
};

// Ce qu'on veut regarder : un émetteur et ses pistes (voir ViewerOptions)
struct WatchRequest {
	std::string sender;  // identifiant de l'émetteur, « moi » pour ce PC
	std::string address; // à défaut, adresse saisie à la main (émetteur ouvert)
	std::string video;
	std::vector<std::string> audio;
	std::string output;
	bool fullscreen = false;
	int screen = 0;
	bool background = false;
};

class Engine {
public:
	Engine();
	~Engine();

	// Réglages à utiliser au démarrage (ligne de commande), sans rien redémarrer
	void preset(const EngineSettings &s);
	// persist : sources et réglages lus et enregistrés sur le disque (faux en ligne de
	// commande, où seuls l'identité et les appairages sont conservés). network : répondre à la
	// découverte et aux appairages (faux pour une visionneuse seule en ligne de commande).
	bool start(bool persist, bool network = true);
	void stop();

	std::vector<SourceChoice> sources();
	void set_sources(const std::vector<SourceChoice> &sources);
	void set_source(const SourceChoice &source, bool on);
	void set_volume(const SourceChoice &source, float volume);
	float volume_of(const SourceChoice &source);
	std::map<std::string, float> volumes();
	// Fenêtres ajoutées par l'utilisateur : gardées comme tuiles même quand elles ne sont pas envoyées
	std::vector<std::string> windows();
	void add_window(const std::string &exe);
	void remove_window(const std::string &exe);
	EngineSettings settings();
	void set_settings(const EngineSettings &s);

	// Pour l'interface : crête du son envoyé par piste, dernière image d'une piste vidéo
	std::map<std::string, float> levels();
	flash::ComPtr<ID3D11Texture2D> last_frame(const std::string &key);
	flash::ComPtr<ID3D11Device> device() { return device_; }
	ObsClient *obs() { return obs_.get(); }

	std::string id();
	std::string name();
	std::string code();
	void new_code();
	std::vector<flash::PeerInfo> receivers();
	void forget_receiver(const std::string &id);

	// Réception : émetteurs du réseau, appairage avec l'un d'eux, visionneuses
	std::vector<flash::SenderAnnounce> discover(bool refresh);
	nlohmann::json senders_json(bool refresh); // trouvés et appairés, pour l'interface
	flash::PairingResult pair(const std::string &address, uint16_t port, const std::string &code);
	void forget_sender(const std::string &id);
	bool watch(const WatchRequest &request, std::string &error);
	void close_view(const std::string &sender);
	size_t views(); // visionneuses ouvertes (les fenêtres fermées sont retirées)

	nlohmann::json status(); // état pour l'interface (récepteurs, pistes, débits, visionneuses)

private:
	enum Part : unsigned { kIdentity = 1, kReceivers = 2, kSenders = 4, kSettings = 8 };

	void load();
	void save(unsigned parts); // data_ tenu ; seules ces parties du fichier sont réécrites
	void start_network();
	void restart_sender();
	void tracks_changed(); // mutex_ tenu : l'annonce de découverte suit les pistes
	bool authorize(const std::string &stream_id, std::string &passphrase);
	std::string announce();
	bool refresh_target(const std::string &sender, flash::SrtOptions &o, int &failures);
	nlohmann::json views_json();

	struct Peer {
		std::string id, name, key, address;
	};

	// mutex_ sérialise les opérations (redémarrages) ; data_ protège les données lues par les
	// fils de découverte, d'appairage et de SRT. Jamais data_ pendant l'arrêt d'un de ces fils.
	std::recursive_mutex mutex_;
	std::mutex data_;
	bool persist_ = false;
	bool network_ = true;
	std::string id_, name_, code_;
	std::string self_key_; // aperçu de ce PC par sa propre visionneuse, le temps de la session
	std::vector<Peer> receivers_;
	std::vector<Peer> senders_; // émetteurs avec lesquels ce PC s'est appairé
	std::vector<SourceChoice> sources_;
	std::map<std::string, float> volumes_; // par source, même quand elle n'est pas envoyée
	std::vector<std::string> windows_;
	bool volumes_dirty_ = false;
	uint64_t volumes_saved_ = 0;
	EngineSettings settings_;

	flash::ComPtr<ID3D11Device> device_;
	std::unique_ptr<flash::Sender> sender_;
	std::unique_ptr<ObsClient> obs_; // scènes et sources d'OBS sur ce PC
	flash::PairingServer pairing_;
	flash::DiscoveryResponder discovery_;
	std::string tracks_json_ = "[]";
	std::vector<flash::TrackStats> last_stats_;
	uint64_t last_stats_ns_ = 0;

	std::mutex discover_mutex_;
	std::vector<flash::SenderAnnounce> seen_;
	uint64_t seen_at_ = 0;

	std::mutex views_mutex_;
	std::map<std::string, std::unique_ptr<Viewer>> views_;
};
