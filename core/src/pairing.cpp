#include "flash/pairing.h"
#include "flash/clock.h"
#include "flash/log.h"
#include "flash/security.h"

#include <winsock2.h>
#include <ws2tcpip.h>

extern "C" {
#include "monocypher.h"
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

using json = nlohmann::json;

namespace flash {

namespace {

constexpr size_t kMaxLine = 2048;
constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxAttemptsPerMinute = 5;

struct WinsockInit {
	WinsockInit()
	{
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
	}
	~WinsockInit() { WSACleanup(); }
};

void winsock()
{
	static WinsockInit init;
}

bool send_json(SOCKET s, const json &j)
{
	const std::string line = j.dump() + "\n";
	return send(s, line.data(), (int)line.size(), 0) == (int)line.size();
}

bool recv_json(SOCKET s, json &out)
{
	std::string line;
	char c;
	while (line.size() < kMaxLine) {
		if (recv(s, &c, 1, 0) != 1)
			return false;
		if (c == '\n') {
			out = json::parse(line, nullptr, false);
			return out.is_object();
		}
		line += c;
	}
	return false;
}

bool get_hex(const json &j, const char *key, uint8_t *out, size_t size)
{
	if (!j.contains(key) || !j[key].is_string())
		return false;
	std::vector<uint8_t> v;
	if (!from_hex(j[key].get<std::string>(), v) || v.size() != size)
		return false;
	std::memcpy(out, v.data(), size);
	return true;
}

void put(crypto_blake2b_ctx &ctx, const void *data, size_t size)
{
	const uint8_t len[4] = {(uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
	crypto_blake2b_update(&ctx, len, 4);
	crypto_blake2b_update(&ctx, (const uint8_t *)data, size);
}

void put(crypto_blake2b_ctx &ctx, const std::string &s)
{
	put(ctx, s.data(), s.size());
}

// Générateur commun, tiré du code et des identités : seul qui connaît le code obtient le même
void generator(uint8_t g[32], const std::string &code, const std::string &sender, const std::string &receiver,
	       const uint8_t nonce[16])
{
	crypto_blake2b_ctx ctx;
	crypto_blake2b_init(&ctx, 32);
	put(ctx, std::string("FLASH-CPace-1"));
	put(ctx, normalize_code(code));
	put(ctx, sender);
	put(ctx, receiver);
	put(ctx, nonce, 16);
	uint8_t h[32];
	crypto_blake2b_final(&ctx, h);
	crypto_elligator_map(g, h);
	crypto_wipe(h, sizeof(h));
}

// Clé de session, liée à tout l'échange
bool session_key(uint8_t isk[32], const uint8_t secret[32], const uint8_t peer[32], const std::string &sender,
		 const std::string &receiver, const uint8_t nonce[16], const uint8_t ya[32], const uint8_t yb[32])
{
	uint8_t k[32];
	crypto_x25519(k, secret, peer);
	static const uint8_t zero[32] = {};
	if (crypto_verify32(k, zero) == 0)
		return false; // point de petit ordre : échange truqué
	crypto_blake2b_ctx ctx;
	crypto_blake2b_keyed_init(&ctx, 32, k, 32);
	put(ctx, std::string("FLASH-ISK"));
	put(ctx, sender);
	put(ctx, receiver);
	put(ctx, nonce, 16);
	put(ctx, ya, 32);
	put(ctx, yb, 32);
	crypto_blake2b_final(&ctx, isk);
	crypto_wipe(k, sizeof(k));
	return true;
}

void derive(uint8_t out[32], const uint8_t isk[32], const char *label)
{
	crypto_blake2b_keyed(out, 32, isk, 32, (const uint8_t *)label, std::strlen(label));
}

void set_timeouts(SOCKET s)
{
	const DWORD ms = kTimeoutMs;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
}

} // namespace

// ---------- Émetteur ----------

PairingServer::~PairingServer()
{
	stop();
}

bool PairingServer::start(const std::string &address, uint16_t port, const PeerInfo &self, Hooks hooks)
{
	winsock();
	self_ = self;
	hooks_ = std::move(hooks);

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, address.c_str(), &addr.sin_addr);
	if (s == INVALID_SOCKET || bind(s, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 4) != 0) {
		FLOG_E("Appairage impossible sur %s:%u (port occupé ?)", address.c_str(), port);
		if (s != INVALID_SOCKET)
			closesocket(s);
		return false;
	}
	listener_ = s;
	running_ = true;
	thread_ = std::thread(&PairingServer::run, this);
	return true;
}

void PairingServer::stop()
{
	if (!running_.exchange(false))
		return;
	closesocket((SOCKET)listener_); // débloque accept()
	if (thread_.joinable())
		thread_.join();
}

void PairingServer::run()
{
	while (running_) {
		SOCKET c = accept((SOCKET)listener_, nullptr, nullptr);
		if (c == INVALID_SOCKET)
			continue;
		set_timeouts(c);
		serve(c); // un appairage à la fois : c'est rare et court
		closesocket(c);
	}
}

bool PairingServer::allow_attempt()
{
	std::lock_guard lock(attempts_mutex_);
	const uint64_t now = now_ns();
	attempts_.erase(std::remove_if(attempts_.begin(), attempts_.end(),
				       [now](uint64_t t) { return now - t > 60'000'000'000ULL; }),
			attempts_.end());
	if (attempts_.size() >= kMaxAttemptsPerMinute)
		return false;
	attempts_.push_back(now);
	return true;
}

void PairingServer::serve(uintptr_t sock)
{
	const SOCKET s = (SOCKET)sock;
	json msg;
	if (!recv_json(s, msg))
		return;
	if (msg.value("type", "") == "qui") {
		send_json(s, {{"type", "moi"}, {"id", self_.id}, {"nom", self_.name}});
		if (!recv_json(s, msg))
			return;
	}
	if (msg.value("type", "") != "appairage")
		return;
	if (!allow_attempt()) {
		send_json(s, {{"type", "erreur"}, {"message", "Trop d'essais, réessayez dans une minute"}});
		return;
	}

	const PeerInfo receiver{msg.value("id", ""), msg.value("nom", "")};
	uint8_t nonce[16], ya[32];
	if (receiver.id.empty() || !get_hex(msg, "nonce", nonce, 16) || !get_hex(msg, "Y", ya, 32))
		return;

	uint8_t g[32], yb_secret[32], yb[32], isk[32];
	generator(g, hooks_.code(), self_.id, receiver.id, nonce);
	random_bytes(yb_secret, 32);
	crypto_x25519(yb, yb_secret, g);
	const bool ok = session_key(isk, yb_secret, ya, self_.id, receiver.id, nonce, ya, yb);
	crypto_wipe(yb_secret, 32);
	if (!ok)
		return;

	uint8_t proof_b[32], proof_a[32], expected_a[32];
	derive(proof_b, isk, "B");
	derive(expected_a, isk, "A");
	if (!send_json(s, {{"type", "defi"}, {"Y", to_hex(yb, 32)}, {"preuve", to_hex(proof_b, 32)}}))
		return;

	json conf;
	const bool confirmed = recv_json(s, conf) && conf.value("type", "") == "confirmation" &&
			       get_hex(conf, "preuve", proof_a, 32) && crypto_verify32(proof_a, expected_a) == 0;
	if (confirmed) {
		uint8_t key[32];
		derive(key, isk, "SRT");
		hooks_.paired(receiver, to_hex(key, 32));
		crypto_wipe(key, 32);
		FLOG_I("Appairé avec « %s »", receiver.name.c_str());
	} else {
		FLOG_W("Appairage refusé pour « %s » : code incorrect", receiver.name.c_str());
	}
	send_json(s, {{"type", "resultat"}, {"ok", confirmed}});
	crypto_wipe(isk, 32);
}

// ---------- Récepteur ----------

PairingResult pair_with_sender(const std::string &host, uint16_t port, const std::string &code, const PeerInfo &self)
{
	winsock();
	PairingResult r;

	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
		r.error = "Émetteur introuvable";
		return r;
	}
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	set_timeouts(s);
	const bool connected = connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
	freeaddrinfo(res);
	if (!connected) {
		closesocket(s);
		r.error = "Émetteur injoignable (l'appli FLASH est-elle lancée ?)";
		return r;
	}

	json msg;
	if (!send_json(s, {{"type", "qui"}}) || !recv_json(s, msg) || msg.value("type", "") != "moi") {
		closesocket(s);
		r.error = "Réponse inattendue de l'émetteur";
		return r;
	}
	r.sender = {msg.value("id", ""), msg.value("nom", "")};

	uint8_t nonce[16], g[32], ya_secret[32], ya[32];
	random_bytes(nonce, 16);
	generator(g, code, r.sender.id, self.id, nonce);
	random_bytes(ya_secret, 32);
	crypto_x25519(ya, ya_secret, g);
	send_json(s, {{"type", "appairage"},
		      {"id", self.id},
		      {"nom", self.name},
		      {"nonce", to_hex(nonce, 16)},
		      {"Y", to_hex(ya, 32)}});

	uint8_t yb[32], proof_b[32], isk[32];
	if (!recv_json(s, msg) || msg.value("type", "") != "defi" || !get_hex(msg, "Y", yb, 32) ||
	    !get_hex(msg, "preuve", proof_b, 32)) {
		r.error = msg.value("type", "") == "erreur" ? msg.value("message", "") : "Réponse inattendue de l'émetteur";
		crypto_wipe(ya_secret, 32);
		closesocket(s);
		return r;
	}
	const bool ok = session_key(isk, ya_secret, yb, r.sender.id, self.id, nonce, ya, yb);
	crypto_wipe(ya_secret, 32);

	uint8_t expected_b[32];
	derive(expected_b, isk, "B");
	if (!ok || crypto_verify32(proof_b, expected_b) != 0) {
		// Code faux : on s'arrête là, l'émetteur n'apprend rien de plus
		r.error = "Code incorrect";
		closesocket(s);
		return r;
	}

	uint8_t proof_a[32], key[32];
	derive(proof_a, isk, "A");
	send_json(s, {{"type", "confirmation"}, {"preuve", to_hex(proof_a, 32)}});
	if (recv_json(s, msg) && msg.value("type", "") == "resultat" && msg.value("ok", false)) {
		derive(key, isk, "SRT");
		r.key_hex = to_hex(key, 32);
		r.ok = true;
		crypto_wipe(key, 32);
	} else {
		r.error = "L'émetteur a refusé l'appairage";
	}
	crypto_wipe(isk, 32);
	closesocket(s);
	return r;
}

} // namespace flash
