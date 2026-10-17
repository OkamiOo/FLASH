#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sockaddr;

// Transport SRT de FLASH. L'émetteur écoute, les récepteurs l'appellent. Une seule
// connexion par couple émetteur/récepteur porte toutes les pistes, dans les deux sens.

namespace flash {

struct SrtOptions {
	std::string address = "127.0.0.1";
	uint16_t port = 9710;
	int latency_ms = 20;
	int peer_idle_ms = 1500; // coupure détectée en 1,5 s : reconnexion rapide
	std::string passphrase;  // vide = sans chiffrement (boucle locale de test)
	std::string stream_id;   // identifiant du récepteur

	// Émetteur : décide pour chaque récepteur (selon son identifiant) s'il est admis et avec
	// quelle phrase secrète. Absent : tout le monde, sans chiffrement.
	std::function<bool(const std::string &stream_id, std::string &passphrase)> authorize;
	// Récepteur : appelé avant chaque tentative de connexion pour mettre à jour l'adresse et
	// la phrase secrète (émetteur qui a changé d'adresse…). Faux : on attend et on réessaie.
	std::function<bool(SrtOptions &options)> refresh;
};

// Initialise libsrt une fois par processus. Dans OBS, libsrt est partagée avec OBS :
// on ne touche ni à son journal ni à son nettoyage.
void srt_init(bool own_logging);

struct SrtLinkStats {
	double rtt_ms = 0;
	double mbps = 0;
	int64_t lost = 0;
	int64_t dropped = 0;
	int64_t retransmitted = 0;
	int latency_ms = 0; // latence de réception négociée avec l'émetteur
};

class SrtServer {
public:
	using OnJoin = std::function<void(int client)>;
	using OnMessage = std::function<void(int client, const uint8_t *data, size_t size)>;

	~SrtServer();

	bool start(const SrtOptions &options, OnJoin on_join, OnMessage on_message);
	void stop();

	// Envoie un message MPEG-TS aux récepteurs qui veulent sa piste (PID du premier paquet ;
	// tables et horloge vont à tous). Un récepteur dont le tampon est plein perd ce message
	// sans freiner les autres.
	void send(const uint8_t *data, size_t size);
	size_t client_count() const;

	// Pistes demandées par un récepteur ; vide = toutes.
	void set_subscription(int client, std::vector<uint16_t> pids);
	// Vrai si au moins un récepteur connecté veut cette piste.
	bool wanted(uint16_t pid) const;

	// Formats vidéo annoncés par un récepteur (bits de flash::formats)
	void set_formats(int client, uint8_t formats);
	// Formats que tous les récepteurs connectés décodent (tous si aucun). Un récepteur qui
	// n'a rien annoncé est attendu pendant grace_ns après sa connexion, puis compte pour legacy.
	uint8_t common_formats(uint64_t grace_ns, uint8_t legacy) const;

private:
	struct Client {
		int sock;
		std::vector<uint16_t> pids;
		uint64_t joined_ns = 0;
		int formats = -1; // -1 : pas encore annoncés
	};

	void run();
	void drop_client(int sock);
	static int listen_hook(void *self, int sock, int hs_version, const ::sockaddr *peer, const char *stream_id);

	SrtOptions options_;
	OnJoin on_join_;
	OnMessage on_message_;
	int listener_ = -1;
	int epoll_ = -1;
	std::atomic<bool> running_{false};
	std::thread thread_;
	mutable std::mutex mutex_;
	std::vector<Client> clients_;
};

class SrtClient {
public:
	using OnData = std::function<void(const uint8_t *data, size_t size, uint64_t arrival_ns)>;
	using OnState = std::function<void(bool connected)>;

	~SrtClient();

	void start(const SrtOptions &options, OnData on_data, OnState on_state);
	void stop();

	bool send(const uint8_t *data, size_t size);
	bool connected() const { return connected_; }
	SrtLinkStats stats();
	int latency_ms() const;

private:
	void run();

	SrtOptions options_;
	OnData on_data_;
	OnState on_state_;
	std::atomic<bool> running_{false};
	std::atomic<bool> connected_{false};
	std::atomic<int> sock_{-1};
	std::thread thread_;
};

} // namespace flash
