#include "flash/srt_link.h"
#include "flash/clock.h"
#include "flash/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <srt/srt.h>

#include <algorithm>

namespace flash {

namespace {

void srt_log_handler(void *, int level, const char *, int, const char *, const char *message)
{
	if (level <= 3) // LOG_ERR et plus grave
		FLOG_W("SRT : %s", message);
}

bool set_int(SRTSOCKET s, SRT_SOCKOPT opt, int value)
{
	return srt_setsockflag(s, opt, &value, sizeof(value)) == 0;
}

bool set_bool(SRTSOCKET s, SRT_SOCKOPT opt, bool value)
{
	return srt_setsockflag(s, opt, &value, sizeof(value)) == 0;
}

void apply_common(SRTSOCKET s, const SrtOptions &o)
{
	const SRT_TRANSTYPE live = SRTT_LIVE;
	srt_setsockflag(s, SRTO_TRANSTYPE, &live, sizeof(live));
	set_int(s, SRTO_LATENCY, o.latency_ms);
	set_int(s, SRTO_PEERIDLETIMEO, o.peer_idle_ms);
	if (!o.passphrase.empty()) {
		srt_setsockflag(s, SRTO_PASSPHRASE, o.passphrase.data(), (int)o.passphrase.size());
		set_int(s, SRTO_PBKEYLEN, 32); // AES-256
	}
}

bool resolve(const std::string &host, uint16_t port, sockaddr_in &out)
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
		return false;
	out = *(const sockaddr_in *)res->ai_addr;
	out.sin_port = htons(port);
	freeaddrinfo(res);
	return true;
}

} // namespace

void srt_init(bool own_logging)
{
	static std::once_flag once;
	std::call_once(once, [own_logging] {
		srt_startup();
		if (own_logging) {
			srt_setloglevel(3);
			srt_setloghandler(nullptr, srt_log_handler);
		}
	});
}

// ---------- Émetteur ----------

SrtServer::~SrtServer()
{
	stop();
}

bool SrtServer::start(const SrtOptions &o, OnJoin on_join, OnMessage on_message)
{
	on_join_ = std::move(on_join);
	on_message_ = std::move(on_message);

	sockaddr_in addr{};
	if (!resolve(o.address, o.port, addr)) {
		FLOG_E("Adresse d'écoute invalide : %s", o.address.c_str());
		return false;
	}

	listener_ = srt_create_socket();
	apply_common(listener_, o);
	set_bool(listener_, SRTO_RCVSYN, false);

	if (srt_bind(listener_, (sockaddr *)&addr, sizeof(addr)) == SRT_ERROR ||
	    srt_listen(listener_, 8) == SRT_ERROR) {
		FLOG_E("Écoute SRT impossible sur %s:%u : %s", o.address.c_str(), o.port, srt_getlasterror_str());
		srt_close(listener_);
		listener_ = -1;
		return false;
	}

	epoll_ = srt_epoll_create();
	const int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;
	srt_epoll_add_usock(epoll_, listener_, &ev);

	running_ = true;
	thread_ = std::thread(&SrtServer::run, this);
	FLOG_I("En écoute SRT sur %s:%u (latence %d ms)", o.address.c_str(), o.port, o.latency_ms);
	return true;
}

void SrtServer::stop()
{
	if (!running_.exchange(false))
		return;
	if (thread_.joinable())
		thread_.join();

	std::lock_guard lock(mutex_);
	for (const Client &c : clients_)
		srt_close(c.sock);
	clients_.clear();
	srt_epoll_release(epoll_);
	srt_close(listener_);
	epoll_ = listener_ = -1;
}

void SrtServer::run()
{
	SRT_EPOLL_EVENT events[16];
	char buf[1500];

	while (running_) {
		const int n = srt_epoll_uwait(epoll_, events, 16, 200);
		for (int i = 0; i < n; i++) {
			const SRTSOCKET s = events[i].fd;

			if (s == listener_) {
				sockaddr_storage peer{};
				int len = sizeof(peer);
				const SRTSOCKET c = srt_accept(listener_, (sockaddr *)&peer, &len);
				if (c == SRT_INVALID_SOCK)
					continue;
				set_bool(c, SRTO_SNDSYN, false);
				set_bool(c, SRTO_RCVSYN, false);
				const int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;
				srt_epoll_add_usock(epoll_, c, &ev);
				{
					std::lock_guard lock(mutex_);
					clients_.push_back({c, {}});
				}
				char host[64] = "?";
				if (peer.ss_family == AF_INET)
					inet_ntop(AF_INET, &((sockaddr_in *)&peer)->sin_addr, host, sizeof(host));
				FLOG_I("Récepteur connecté : %s", host);
				if (on_join_)
					on_join_(c);
				continue;
			}

			if (events[i].events & SRT_EPOLL_ERR) {
				drop_client(s);
				continue;
			}

			for (;;) {
				const int r = srt_recvmsg2(s, buf, sizeof(buf), nullptr);
				if (r > 0) {
					if (on_message_)
						on_message_(s, (const uint8_t *)buf, (size_t)r);
					continue;
				}
				if (srt_getlasterror(nullptr) != SRT_EASYNCRCV)
					drop_client(s);
				break;
			}
		}
	}
}

void SrtServer::drop_client(int s)
{
	{
		std::lock_guard lock(mutex_);
		auto it = std::find_if(clients_.begin(), clients_.end(), [s](const Client &c) { return c.sock == s; });
		if (it == clients_.end())
			return;
		clients_.erase(it);
	}
	srt_epoll_remove_usock(epoll_, s);
	srt_close(s);
	FLOG_I("Récepteur déconnecté");
}

namespace {
bool is_system_pid(uint16_t pid)
{
	return pid == 0 || pid == 0x1000 || pid == 0x01FF || pid == 0x0300;
}
} // namespace

void SrtServer::send(const uint8_t *data, size_t size)
{
	if (size < 3)
		return;
	const uint16_t pid = (uint16_t)(((data[1] & 0x1F) << 8) | data[2]);
	const bool everyone = is_system_pid(pid);

	std::lock_guard lock(mutex_);
	for (const Client &c : clients_) {
		if (everyone || c.pids.empty() || std::find(c.pids.begin(), c.pids.end(), pid) != c.pids.end())
			srt_sendmsg2(c.sock, (const char *)data, (int)size, nullptr); // échec = perdu pour lui seul
	}
}

void SrtServer::set_subscription(int client, std::vector<uint16_t> pids)
{
	std::lock_guard lock(mutex_);
	for (Client &c : clients_) {
		if (c.sock == client)
			c.pids = std::move(pids);
	}
}

bool SrtServer::wanted(uint16_t pid) const
{
	std::lock_guard lock(mutex_);
	for (const Client &c : clients_) {
		if (c.pids.empty() || std::find(c.pids.begin(), c.pids.end(), pid) != c.pids.end())
			return true;
	}
	return false;
}

size_t SrtServer::client_count() const
{
	std::lock_guard lock(mutex_);
	return clients_.size();
}

// ---------- Récepteur ----------

SrtClient::~SrtClient()
{
	stop();
}

void SrtClient::start(const SrtOptions &o, OnData on_data, OnState on_state)
{
	stop();
	options_ = o;
	on_data_ = std::move(on_data);
	on_state_ = std::move(on_state);
	running_ = true;
	thread_ = std::thread(&SrtClient::run, this);
}

void SrtClient::stop()
{
	if (!running_.exchange(false))
		return;
	if (thread_.joinable())
		thread_.join();
}

bool SrtClient::send(const uint8_t *data, size_t size)
{
	const int s = sock_;
	return s >= 0 && connected_ && srt_sendmsg2(s, (const char *)data, (int)size, nullptr) > 0;
}

SrtLinkStats SrtClient::stats()
{
	SrtLinkStats out;
	const int s = sock_;
	SRT_TRACEBSTATS perf{};
	if (s >= 0 && connected_ && srt_bstats(s, &perf, 1) == 0) {
		out.rtt_ms = perf.msRTT;
		out.mbps = perf.mbpsRecvRate;
		out.lost = perf.pktRcvLoss;
		out.dropped = perf.pktRcvDrop;
		out.retransmitted = perf.pktRcvRetrans;
	}
	return out;
}

void SrtClient::run()
{
	char buf[1500];
	bool warned = false;

	while (running_) {
		sockaddr_in addr{};
		if (!resolve(options_.address, options_.port, addr)) {
			if (!warned)
				FLOG_W("Émetteur introuvable : %s", options_.address.c_str());
			warned = true;
			sleep_until_ns(now_ns() + 1000000000ULL);
			continue;
		}

		const SRTSOCKET s = srt_create_socket();
		apply_common(s, options_);
		set_int(s, SRTO_CONNTIMEO, 1000);
		set_int(s, SRTO_RCVTIMEO, 200);
		if (!options_.stream_id.empty())
			srt_setsockflag(s, SRTO_STREAMID, options_.stream_id.data(), (int)options_.stream_id.size());

		if (srt_connect(s, (sockaddr *)&addr, sizeof(addr)) == SRT_ERROR) {
			if (!warned)
				FLOG_I("Connexion à %s:%u impossible pour l'instant, nouvel essai en continu",
				       options_.address.c_str(), options_.port);
			warned = true;
			srt_close(s);
			sleep_until_ns(now_ns() + 500000000ULL);
			continue;
		}

		warned = false;
		sock_ = s;
		connected_ = true;
		FLOG_I("Connecté à %s:%u", options_.address.c_str(), options_.port);
		if (on_state_)
			on_state_(true);

		while (running_) {
			const int r = srt_recvmsg2(s, buf, sizeof(buf), nullptr);
			if (r > 0) {
				on_data_((const uint8_t *)buf, (size_t)r, now_ns());
				continue;
			}
			if (srt_getlasterror(nullptr) != SRT_EASYNCRCV)
				break; // coupure : on se reconnecte
		}

		connected_ = false;
		sock_ = -1;
		srt_close(s);
		if (on_state_)
			on_state_(false);
		if (running_)
			FLOG_I("Connexion perdue, reconnexion…");
	}
}

} // namespace flash
