#include "flash/discovery.h"
#include "flash/clock.h"
#include "flash/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

using json = nlohmann::json;

namespace flash {

namespace {

constexpr char kGroup[] = "239.255.71.10";

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

struct Interface {
	in_addr address;
	in_addr broadcast;
};

// Adresses IPv4 des cartes réseau actives, avec l'adresse de diffusion de leur sous-réseau
std::vector<Interface> interfaces()
{
	std::vector<Interface> out;
	ULONG size = 16 * 1024;
	std::vector<uint8_t> buf(size);
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	if (GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size) == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		if (GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size) != NO_ERROR)
			return out;
	}
	for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			const auto *sin = (const sockaddr_in *)u->Address.lpSockaddr;
			if (sin->sin_family != AF_INET)
				continue;
			const ULONG mask = u->OnLinkPrefixLength ? htonl(~0UL << (32 - u->OnLinkPrefixLength)) : 0;
			Interface i;
			i.address = sin->sin_addr;
			i.broadcast.s_addr = sin->sin_addr.s_addr | ~mask;
			out.push_back(i);
		}
	}
	return out;
}

// Préférence d'adresse : le même PC, puis le réseau local, puis le reste (VPN…)
int rank(const std::string &address)
{
	if (address.rfind("127.", 0) == 0)
		return 0;
	if (address.rfind("192.168.", 0) == 0 || address.rfind("10.", 0) == 0)
		return 1;
	if (address.rfind("172.", 0) == 0) {
		const int second = std::atoi(address.c_str() + 4);
		if (second >= 16 && second <= 31)
			return 1;
	}
	return 2;
}

} // namespace

std::string make_announce(const std::string &id, const std::string &name, uint16_t srt_port, bool open,
			  const std::string &tracks_json)
{
	json j = {{"flash", 1}, {"type", "emetteur"}, {"id", id}, {"nom", name}, {"srt", srt_port},
		  {"appairage", kPairingPort}, {"ouvert", open}};
	json tracks = json::parse(tracks_json, nullptr, false);
	j["pistes"] = tracks.is_array() ? tracks : json::array();
	return j.dump();
}

// ---------- Émetteur ----------

DiscoveryResponder::~DiscoveryResponder()
{
	stop();
}

bool DiscoveryResponder::start(const std::string &address, std::function<std::string()> announce)
{
	winsock();
	announce_ = std::move(announce);

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	const BOOL yes = TRUE;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
	const DWORD timeout = 200;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kDiscoveryPort);
	inet_pton(AF_INET, address.c_str(), &addr.sin_addr);
	if (bind(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
		FLOG_E("Découverte impossible sur %s:%u", address.c_str(), kDiscoveryPort);
		closesocket(s);
		return false;
	}
	if (addr.sin_addr.s_addr == INADDR_ANY) {
		for (const Interface &i : interfaces()) {
			ip_mreq m{};
			inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
			m.imr_interface = i.address;
			setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof(m));
		}
	}

	sock_ = s;
	running_ = true;
	thread_ = std::thread(&DiscoveryResponder::run, this);
	return true;
}

void DiscoveryResponder::stop()
{
	if (!running_.exchange(false))
		return;
	if (thread_.joinable())
		thread_.join();
	closesocket((SOCKET)sock_);
}

void DiscoveryResponder::run()
{
	char buf[1500];
	while (running_) {
		sockaddr_in from{};
		int len = sizeof(from);
		const int n = recvfrom((SOCKET)sock_, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &len);
		if (n <= 0)
			continue;
		const json q = json::parse(std::string(buf, n), nullptr, false);
		if (!q.is_object() || q.value("flash", 0) != 1 || q.value("type", "") != "recherche")
			continue;
		const std::string reply = announce_();
		sendto((SOCKET)sock_, reply.data(), (int)reply.size(), 0, (sockaddr *)&from, len);
	}
}

// ---------- Récepteur ----------

std::vector<SenderAnnounce> discover_senders(int timeout_ms)
{
	winsock();
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	const BOOL yes = TRUE;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
	const DWORD ttl = 1;
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
	sockaddr_in local{};
	local.sin_family = AF_INET;
	bind(s, (sockaddr *)&local, sizeof(local));

	const std::string query = json{{"flash", 1}, {"type", "recherche"}}.dump();
	auto send_to = [&](in_addr a) {
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(kDiscoveryPort);
		to.sin_addr = a;
		sendto(s, query.data(), (int)query.size(), 0, (sockaddr *)&to, sizeof(to));
	};
	auto ask = [&] {
		in_addr a;
		inet_pton(AF_INET, "127.0.0.1", &a);
		send_to(a);
		inet_pton(AF_INET, "255.255.255.255", &a);
		send_to(a);
		in_addr group;
		inet_pton(AF_INET, kGroup, &group);
		for (const Interface &i : interfaces()) {
			send_to(i.broadcast);
			setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&i.address, sizeof(i.address));
			send_to(group);
		}
	};

	std::map<std::string, SenderAnnounce> found;
	const uint64_t start = now_ns();
	const uint64_t deadline = start + (uint64_t)timeout_ms * 1'000'000ULL;
	bool asked_again = false;
	ask();
	for (;;) {
		const uint64_t now = now_ns();
		if (now >= deadline)
			break;
		if (!asked_again && now - start > 150'000'000ULL) {
			ask(); // un paquet UDP peut se perdre : on redemande une fois
			asked_again = true;
		}
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(s, &fds);
		timeval tv{0, 50'000};
		if (select(0, &fds, nullptr, nullptr, &tv) <= 0)
			continue;

		char buf[4096];
		sockaddr_in from{};
		int len = sizeof(from);
		const int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr *)&from, &len);
		if (n <= 0)
			continue;
		const json j = json::parse(std::string(buf, n), nullptr, false);
		if (!j.is_object() || j.value("flash", 0) != 1 || j.value("type", "") != "emetteur")
			continue;

		char host[64];
		inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
		SenderAnnounce a;
		a.id = j.value("id", "");
		a.name = j.value("nom", "");
		a.address = host;
		a.srt_port = (uint16_t)j.value("srt", 9710);
		a.pairing_port = (uint16_t)j.value("appairage", (int)kPairingPort);
		a.open = j.value("ouvert", false);
		if (j.contains("pistes") && j["pistes"].is_array()) {
			for (const auto &t : j["pistes"]) {
				if (t.is_object())
					a.tracks.push_back({(uint16_t)t.value("pid", 0), t.value("type", "") == "video", t.value("nom", "")});
			}
		}
		if (a.id.empty())
			continue;
		auto it = found.find(a.id);
		if (it == found.end() || rank(a.address) < rank(it->second.address))
			found[a.id] = a;
	}
	closesocket(s);

	std::vector<SenderAnnounce> out;
	for (auto &[id, a] : found)
		out.push_back(a);
	return out;
}

} // namespace flash
