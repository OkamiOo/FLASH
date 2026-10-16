#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Appairage par code d'invitation, échange CPace sur X25519 (TCP, port 9712 par défaut).
// Le code ne circule jamais : chacun prouve qu'il le connaît sans le révéler, et une écoute du
// réseau ne permet pas de le retrouver hors ligne. Chaque essai en direct ne teste qu'un code,
// et l'émetteur n'en accepte que 5 par minute. Résultat : une clé de 256 bits propre au couple
// émetteur/récepteur, qui sert ensuite de phrase secrète SRT.

namespace flash {

struct PeerInfo {
	std::string id;
	std::string name;
};

class PairingServer {
public:
	struct Hooks {
		std::function<std::string()> code; // code d'invitation courant
		std::function<void(const PeerInfo &receiver, const std::string &key_hex)> paired;
	};

	~PairingServer();
	bool start(const std::string &address, uint16_t port, const PeerInfo &self, Hooks hooks);
	void stop();

private:
	void run();
	void serve(uintptr_t sock);
	bool allow_attempt();

	PeerInfo self_;
	Hooks hooks_;
	uintptr_t listener_ = ~(uintptr_t)0;
	std::atomic<bool> running_{false};
	std::thread thread_;
	std::mutex attempts_mutex_;
	std::vector<uint64_t> attempts_;
};

struct PairingResult {
	bool ok = false;
	std::string error;
	PeerInfo sender;
	std::string key_hex;
};

// Côté récepteur : appaire avec l'émetteur joignable à cette adresse, avec le code qu'il affiche.
PairingResult pair_with_sender(const std::string &host, uint16_t port, const std::string &code, const PeerInfo &self);

} // namespace flash
