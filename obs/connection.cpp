#include "connection.h"
#include "audio_track.h"
#include "measure.h"
#include "video_track.h"

#include <flash/clock.h>
#include <flash/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace {
std::mutex g_registry_mutex;
std::map<std::string, std::weak_ptr<Connection>> g_registry;

bool is_video(flash::Codec c)
{
	return c == flash::Codec::H264 || c == flash::Codec::HEVC;
}
} // namespace

std::shared_ptr<Connection> Connection::get(const std::string &address, uint16_t port)
{
	const std::string key = address + ":" + std::to_string(port);
	std::lock_guard lock(g_registry_mutex);
	if (auto it = g_registry.find(key); it != g_registry.end()) {
		if (auto existing = it->second.lock())
			return existing;
	}
	std::shared_ptr<Connection> conn(new Connection(address, port));
	g_registry[key] = conn;
	return conn;
}

Connection::Connection(std::string address, uint16_t port) : address_(std::move(address)), port_(port)
{
	flash::SrtOptions options;
	options.address = address_;
	options.port = port_;
	receiver_.start(
		options, [this](const std::vector<flash::TsStream> &streams) { on_streams(streams); },
		[this](const flash::MediaPacket &pkt) { on_packet(pkt); }, [this](bool connected) { on_state(connected); });
}

Connection::~Connection()
{
	receiver_.stop();
	std::lock_guard lock(mutex_);
	video_.clear();
	audio_.clear();
}

uint64_t Connection::video_interval_ns()
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.fps_num)
		return 16'666'667;
	return 1'000'000'000ULL * ovi.fps_den / ovi.fps_num;
}

void Connection::add_source(obs_source_t *source, bool video, int track)
{
	std::lock_guard lock(mutex_);
	subs_.push_back({source, video, track});
	rebind();
}

void Connection::remove_source(obs_source_t *source)
{
	std::lock_guard lock(mutex_);
	subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [source](const Subscription &s) { return s.source == source; }),
		    subs_.end());
	rebind();
}

void Connection::request_keyframe(uint16_t pid)
{
	const uint64_t now = flash::now_ns();
	uint64_t prev = last_keyframe_request_.load();
	if (now - prev < 250'000'000ULL || !last_keyframe_request_.compare_exchange_strong(prev, now))
		return;
	receiver_.request_keyframe(pid);
}

// Associe chaque source à sa piste d'après la table des pistes de l'émetteur, crée les
// décodeurs nécessaires et arrête ceux que plus personne n'utilise.
void Connection::rebind()
{
	std::map<uint16_t, std::vector<obs_source_t *>> want_video, want_audio;
	std::map<uint16_t, flash::Codec> codecs;

	for (const Subscription &sub : subs_) {
		int n = 0;
		for (const flash::TsStream &st : streams_) {
			const bool match = sub.video ? is_video(st.codec) : st.codec == flash::Codec::Opus;
			if (match && ++n == sub.track) {
				(sub.video ? want_video : want_audio)[st.pid].push_back(sub.source);
				codecs[st.pid] = st.codec;
				break;
			}
		}
	}

	for (auto it = video_.begin(); it != video_.end();)
		it = want_video.count(it->first) ? std::next(it) : video_.erase(it);
	for (auto it = audio_.begin(); it != audio_.end();)
		it = want_audio.count(it->first) ? std::next(it) : audio_.erase(it);

	for (auto &[pid, sources] : want_video) {
		auto &track = video_[pid];
		if (!track)
			track = std::make_unique<VideoTrack>(*this, pid, codecs[pid]);
		track->set_sources(std::move(sources));
	}
	for (auto &[pid, sources] : want_audio) {
		auto &track = audio_[pid];
		if (!track)
			track = std::make_unique<AudioTrack>(*this, pid);
		track->set_sources(std::move(sources));
	}

	phase_pid = video_.empty() ? 0 : video_.begin()->first;

	// L'émetteur n'envoie (et n'encode) que les pistes qu'on utilise. Aucune : PID nul.
	std::vector<uint16_t> pids;
	for (const auto &[pid, track] : video_)
		pids.push_back(pid);
	for (const auto &[pid, track] : audio_)
		pids.push_back(pid);
	if (pids.empty())
		pids.push_back(0x1FFF);
	receiver_.subscribe(std::move(pids));
}

// Métadonnées de l'émetteur : nom du PC et nom de chaque piste
void Connection::on_meta(std::span<const uint8_t> data)
{
	const std::string text(data.begin(), data.end());
	if (text == meta_)
		return;
	meta_ = text;

	const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
	if (!j.is_object() || !j.contains("pistes") || !j["pistes"].is_array())
		return;
	std::string line;
	for (const auto &t : j["pistes"]) {
		if (!t.is_object())
			continue;
		const std::string type = t.value("type", "");
		line += (line.empty() ? "" : ", ") + std::string(type == "video" ? "vidéo" : "son") + " « " +
			t.value("nom", "?") + " »";
	}
	FLOG_I("%s:%u, émetteur « %s » : %s", address_.c_str(), port_, j.value("nom", "?").c_str(), line.c_str());
}

void Connection::on_streams(const std::vector<flash::TsStream> &streams)
{
	std::lock_guard lock(mutex_);
	streams_ = streams;
	int videos = 0, sounds = 0;
	for (const flash::TsStream &st : streams) {
		videos += is_video(st.codec);
		sounds += st.codec == flash::Codec::Opus;
	}
	FLOG_I("%s:%u : %d vidéo(s), %d son(s)", address_.c_str(), port_, videos, sounds);
	rebind();
}

void Connection::on_packet(const flash::MediaPacket &pkt)
{
	if (pkt.codec == flash::Codec::Meta) {
		std::lock_guard lock(mutex_);
		on_meta(pkt.data);
		return;
	}

	// Chaque piste reçue nourrit l'horloge commune
	clock_.observe(pkt.pts_ns, pkt.arrival_ns);

	std::lock_guard lock(mutex_);
	const int64_t lag = (int64_t)(pkt.arrival_ns - pkt.pts_ns);
	int64_t &max_lag = is_video(pkt.codec) ? max_video_lag_ : max_audio_lag_;
	max_lag = lag > max_lag ? lag : max_lag;
	if (is_video(pkt.codec)) {
		if (auto it = video_.find(pkt.pid); it != video_.end())
			it->second->push(pkt);
	} else if (pkt.codec == flash::Codec::Opus) {
		if (auto it = audio_.find(pkt.pid); it != audio_.end())
			it->second->push(pkt);
	}

	if (pkt.arrival_ns - last_report_ >= 1'000'000'000ULL) {
		last_report_ = pkt.arrival_ns;
		measure::clock(clock_.offset_ns(), phase_ns.load(), max_video_lag_, max_audio_lag_);
		max_video_lag_ = max_audio_lag_ = INT64_MIN;
	}
}

void Connection::on_state(bool connected)
{
	if (connected)
		return;
	// Émetteur perdu : on oublie son horloge et ses pistes, tout sera reconstruit
	// à la reconnexion.
	clock_.reset();
	phase_ns = 0;
	std::lock_guard lock(mutex_);
	streams_.clear();
	rebind();
}
