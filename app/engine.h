#pragma once

#include <flash/discovery.h>
#include <flash/pairing.h>
#include <flash/sender.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Le cœur de l'appli : réglages, sources choisies, envoi, appairage et découverte.
// Les réglages vivent dans %APPDATA%\FLASH\reglages.json ; les clés des récepteurs y sont
// chiffrées pour l'utilisateur Windows (DPAPI).

struct SourceChoice {
	std::string kind; // mire, ecran, fenetre, appli, systeme, micro
	std::string value;
	bool operator==(const SourceChoice &) const = default;
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

class Engine {
public:
	Engine();
	~Engine();

	// Réglages à utiliser au démarrage (ligne de commande), sans rien redémarrer
	void preset(const EngineSettings &s);
	// persist : sources et réglages lus et enregistrés sur le disque (faux en ligne de
	// commande, où seuls l'identité et les récepteurs appairés sont conservés)
	bool start(bool persist);
	void stop();

	std::vector<SourceChoice> sources();
	void set_sources(const std::vector<SourceChoice> &sources);
	EngineSettings settings();
	void set_settings(const EngineSettings &s);

	std::string id();
	std::string name();
	std::string code();
	void new_code();
	std::vector<flash::PeerInfo> receivers();
	void forget_receiver(const std::string &id);

	nlohmann::json status(); // état pour l'interface (récepteurs, pistes, débits)

private:
	void load();
	void save(); // data_ tenu
	void start_network();
	void restart_sender();
	bool authorize(const std::string &stream_id, std::string &passphrase);
	std::string announce();

	struct Receiver {
		std::string id, name, key;
	};

	// mutex_ sérialise les opérations (redémarrages) ; data_ protège les données lues par les
	// fils de découverte, d'appairage et de SRT. Jamais data_ pendant l'arrêt d'un de ces fils.
	std::recursive_mutex mutex_;
	std::mutex data_;
	bool persist_ = false;
	std::string path_;
	std::string id_, name_, code_;
	std::vector<Receiver> receivers_;
	std::vector<SourceChoice> sources_;
	EngineSettings settings_;

	flash::ComPtr<ID3D11Device> device_;
	std::unique_ptr<flash::Sender> sender_;
	flash::PairingServer pairing_;
	flash::DiscoveryResponder discovery_;
	std::string tracks_json_ = "[]";
	std::vector<flash::TrackStats> last_stats_;
	uint64_t last_stats_ns_ = 0;
};
