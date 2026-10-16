#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Découverte en réseau local (UDP 9711). Le récepteur pose la question à tout le réseau
// (diffusion et multicast, plus 127.0.0.1 pour un émetteur sur le même PC) ; chaque émetteur
// répond directement à celui qui demande. Windows laisse passer une réponse à une diffusion
// pendant quelques secondes : côté OBS, aucune règle de pare-feu n'est nécessaire.

namespace flash {

constexpr uint16_t kDiscoveryPort = 9711;
constexpr uint16_t kPairingPort = 9712;

struct AnnouncedTrack {
	uint16_t pid = 0;
	bool video = false;
	std::string name;
};

struct SenderAnnounce {
	std::string id;
	std::string name;
	std::string address; // adresse d'où vient la réponse
	uint16_t srt_port = 9710;
	uint16_t pairing_port = kPairingPort;
	bool open = false; // accepte les récepteurs sans appairage
	std::vector<AnnouncedTrack> tracks;
};

// Côté émetteur : répond aux questions avec l'annonce JSON courante.
class DiscoveryResponder {
public:
	~DiscoveryResponder();
	bool start(const std::string &address, std::function<std::string()> announce);
	void stop();

private:
	void run();

	std::function<std::string()> announce_;
	uintptr_t sock_ = ~(uintptr_t)0;
	std::atomic<bool> running_{false};
	std::thread thread_;
};

// Côté récepteur : interroge le réseau et rend les émetteurs qui ont répondu, un par
// identifiant (l'adresse locale d'abord, puis le réseau local, puis les autres).
std::vector<SenderAnnounce> discover_senders(int timeout_ms);

// Annonce JSON d'un émetteur (le même format que les métadonnées du flux, plus les ports)
std::string make_announce(const std::string &id, const std::string &name, uint16_t srt_port, bool open,
			  const std::string &tracks_json);

} // namespace flash
