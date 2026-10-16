#include "measure.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Les lignes s'accumulent en mémoire et un fil les écrit toutes les 200 ms : aucun fil
// temps réel n'attend jamais le disque.

namespace measure {

namespace {

std::once_flag g_once;
std::atomic<bool> g_enabled{false};
std::mutex g_mutex;
std::condition_variable g_cv;
std::string g_pending;
bool g_stop = false;
std::thread g_writer;
FILE *g_file = nullptr;

void writer()
{
	std::unique_lock lock(g_mutex);
	for (;;) {
		g_cv.wait_for(lock, std::chrono::milliseconds(200), [] { return g_stop; });
		std::string chunk;
		chunk.swap(g_pending);
		const bool stop = g_stop;
		lock.unlock();
		if (!chunk.empty()) {
			std::fwrite(chunk.data(), 1, chunk.size(), g_file);
			std::fflush(g_file);
		}
		if (stop)
			return;
		lock.lock();
	}
}

void init()
{
	char path[1024];
	size_t len = 0;
	if (getenv_s(&len, path, sizeof(path), "FLASH_MESURE") != 0 || len <= 1)
		return;
	g_file = std::fopen(path, "w");
	if (!g_file)
		return;
	std::fputs("type,a,b,c,d,e,f,g\n", g_file);
	g_writer = std::thread(writer);
	g_enabled = true;
}

void add(const char *format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	const int n = std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n <= 0)
		return;
	std::lock_guard lock(g_mutex);
	g_pending.append(line, (size_t)n);
}

} // namespace

bool enabled()
{
	std::call_once(g_once, init);
	return g_enabled;
}

void frame(uint32_t n, uint64_t capture_ns, uint64_t arrival_ns, uint64_t decoded_ns, uint64_t release_ns,
	   uint64_t key, bool flash, uint64_t pts_ns)
{
	if (enabled())
		add("V,%u,%llu,%llu,%llu,%llu,%llu,%d,%llu\n", n, (unsigned long long)capture_ns,
		    (unsigned long long)arrival_ns, (unsigned long long)decoded_ns, (unsigned long long)release_ns,
		    (unsigned long long)key, flash ? 1 : 0, (unsigned long long)pts_ns);
}

struct Shown {
	uint64_t released = 0;
	uint64_t logged = 0;
};
std::mutex g_shown_mutex;
std::unordered_map<const void *, Shown> g_shown;

void released(const void *source, uint64_t key)
{
	if (!enabled())
		return;
	std::lock_guard lock(g_shown_mutex);
	g_shown[source].released = key;
}

void tick(const void *source, uint64_t tick_ns)
{
	if (!enabled())
		return;
	uint64_t key;
	{
		std::lock_guard lock(g_shown_mutex);
		Shown &s = g_shown[source];
		if (!s.released || s.released == s.logged)
			return;
		key = s.logged = s.released;
	}
	add("D,%llu,%llu\n", (unsigned long long)key, (unsigned long long)tick_ns);
}

void beep(uint64_t onset_ns)
{
	if (enabled())
		add("B,%llu\n", (unsigned long long)onset_ns);
}

void clock(int64_t offset_ns, int64_t phase_ns, int64_t video_lag_ns, int64_t audio_lag_ns)
{
	if (enabled())
		add("C,%lld,%lld,%lld,%lld\n", (long long)offset_ns, (long long)phase_ns, (long long)video_lag_ns,
		    (long long)audio_lag_ns);
}

void shutdown()
{
	if (!g_enabled)
		return;
	{
		std::lock_guard lock(g_mutex);
		g_stop = true;
	}
	g_cv.notify_all();
	g_writer.join();
	std::fclose(g_file);
	g_file = nullptr;
	g_enabled = false;
}

} // namespace measure
