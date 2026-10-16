#include "flash/log.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Les messages partent dans un fil à part : écrire dans un fichier peut bloquer des
// dizaines de millisecondes (antivirus, disque), ce qu'un fil temps réel ne doit jamais subir.

namespace flash {

namespace {

void default_sink(LogLevel level, const char *message)
{
	static const char *names[] = {"debug", "info", "attention", "erreur"};
	std::fprintf(stderr, "[%s] %s\n", names[(int)level], message);
	std::fflush(stderr);
}

struct Logger {
	std::atomic<LogSink> sink{default_sink};
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::pair<LogLevel, std::string>> queue;
	std::thread thread;
	bool stop = false;

	void run()
	{
		std::unique_lock lock(mutex);
		for (;;) {
			cv.wait(lock, [this] { return stop || !queue.empty(); });
			while (!queue.empty()) {
				auto [level, text] = std::move(queue.front());
				queue.pop_front();
				lock.unlock();
				sink.load()(level, text.c_str());
				lock.lock();
			}
			if (stop)
				return;
		}
	}
};

Logger &logger()
{
	static Logger l;
	return l;
}

} // namespace

void set_log_sink(LogSink sink)
{
	logger().sink = sink ? sink : default_sink;
}

void log(LogLevel level, const char *format, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	Logger &l = logger();
	{
		std::lock_guard lock(l.mutex);
		if (l.stop)
			return;
		if (!l.thread.joinable())
			l.thread = std::thread(&Logger::run, &l);
		if (l.queue.size() < 1000)
			l.queue.emplace_back(level, buf);
	}
	l.cv.notify_one();
}

void shutdown_log()
{
	Logger &l = logger();
	{
		std::lock_guard lock(l.mutex);
		l.stop = true;
	}
	l.cv.notify_one();
	if (l.thread.joinable())
		l.thread.join();
}

} // namespace flash
