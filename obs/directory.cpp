#include "directory.h"

#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/security.h>

#include <obs-module.h>
#include <util/bmem.h>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <mutex>

using json = nlohmann::json;

namespace directory {

namespace {

constexpr uint64_t kFreshNs = 3'000'000'000ULL;

std::mutex g_mutex;
bool g_loaded = false;
std::string g_id;
std::vector<Paired> g_paired;
std::vector<flash::SenderAnnounce> g_seen;
uint64_t g_seen_at = 0;

std::wstring config_file()
{
	char *path = obs_module_config_path("flash.json");
	std::wstring w = flash::to_wide(path ? path : "flash.json");
	bfree(path);
	const size_t slash = w.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		SHCreateDirectoryExW(nullptr, w.substr(0, slash).c_str(), nullptr);
	return w;
}

void save_locked()
{
	json j = {{"id", g_id}, {"emetteurs", json::array()}};
	for (const Paired &p : g_paired)
		j["emetteurs"].push_back({{"id", p.id}, {"nom", p.name}, {"cle", flash::protect_secret(p.key)}});
	const std::wstring path = config_file();
	const std::wstring tmp = path + L".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << j.dump(2);
	}
	MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void load_locked()
{
	if (g_loaded)
		return;
	g_loaded = true;
	std::ifstream in(config_file());
	const json j = in ? json::parse(in, nullptr, false) : json();
	if (j.is_object()) {
		g_id = j.value("id", "");
		if (j.contains("emetteurs") && j["emetteurs"].is_array()) {
			for (const auto &e : j["emetteurs"]) {
				const std::string key = flash::unprotect_secret(e.value("cle", ""));
				if (!key.empty())
					g_paired.push_back({e.value("id", ""), e.value("nom", ""), key});
			}
		}
	}
	if (g_id.empty()) {
		g_id = flash::new_device_id();
		save_locked();
	}
}

} // namespace

std::string self_id()
{
	std::lock_guard lock(g_mutex);
	load_locked();
	return g_id;
}

std::string self_name()
{
	wchar_t name[256] = L"PC";
	DWORD size = 256;
	GetComputerNameW(name, &size);
	return flash::to_utf8(name) + " (OBS)";
}

std::vector<flash::SenderAnnounce> senders(bool refresh)
{
	{
		std::lock_guard lock(g_mutex);
		if (!refresh && g_seen_at && flash::now_ns() - g_seen_at < kFreshNs)
			return g_seen;
	}
	auto found = flash::discover_senders(400);
	std::lock_guard lock(g_mutex);
	g_seen = std::move(found);
	g_seen_at = flash::now_ns();
	return g_seen;
}

std::optional<flash::SenderAnnounce> find(const std::string &id, bool refresh)
{
	for (const flash::SenderAnnounce &s : senders(refresh)) {
		if (s.id == id)
			return s;
	}
	return std::nullopt;
}

std::vector<Paired> paired()
{
	std::lock_guard lock(g_mutex);
	load_locked();
	return g_paired;
}

std::optional<Paired> paired_with(const std::string &id)
{
	std::lock_guard lock(g_mutex);
	load_locked();
	for (const Paired &p : g_paired) {
		if (p.id == id)
			return p;
	}
	return std::nullopt;
}

void remember(const Paired &p)
{
	std::lock_guard lock(g_mutex);
	load_locked();
	g_paired.erase(std::remove_if(g_paired.begin(), g_paired.end(), [&](const Paired &x) { return x.id == p.id; }),
		       g_paired.end());
	g_paired.push_back(p);
	save_locked();
}

void forget(const std::string &id)
{
	std::lock_guard lock(g_mutex);
	load_locked();
	g_paired.erase(std::remove_if(g_paired.begin(), g_paired.end(), [&](const Paired &x) { return x.id == id; }),
		       g_paired.end());
	save_locked();
}

} // namespace directory
