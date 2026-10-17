#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// MPEG-TS de FLASH : du TS standard (VLC et FFmpeg savent le lire), avec deux particularités.
// - Un PID de PCR dédié, émis en continu, pour qu'un envoi sans vidéo garde une horloge.
// - Le dernier paquet de chaque PES vidéo porte un marqueur dans les données privées du
//   champ d'adaptation : le récepteur livre l'image tout de suite, sans attendre la suivante.
// L'AV1 n'a pas de stream_type à lui : PES privé (0x06) avec un descripteur d'enregistrement
// « AV01 », une unité temporelle (suite d'OBU avec leur taille) par PES.

namespace flash {

enum class Codec : uint8_t { H264 = 1, HEVC = 2, Opus = 3, Meta = 4, AV1 = 5 };

inline bool is_video(Codec c)
{
	return c == Codec::H264 || c == Codec::HEVC || c == Codec::AV1;
}

// Formats vidéo qu'un encodeur ou un décodeur gère, en bits : c'est aussi ce que chaque
// récepteur annonce à l'émetteur par la voie de retour.
namespace formats {
constexpr uint8_t kH264 = 0x01;
constexpr uint8_t kHEVC = 0x02;
constexpr uint8_t kAV1 = 0x04;
// Récepteur d'avant l'échange des formats : il ne savait décoder que ces deux-là
constexpr uint8_t kLegacy = kH264 | kHEVC;
} // namespace formats

inline uint8_t format_bit(Codec c)
{
	return c == Codec::H264 ? formats::kH264 : c == Codec::HEVC ? formats::kHEVC : c == Codec::AV1 ? formats::kAV1 : 0;
}

inline const char *codec_name(Codec c)
{
	return c == Codec::HEVC ? "HEVC" : c == Codec::AV1 ? "AV1" : "H.264";
}

struct Track {
	uint16_t pid = 0;
	Codec codec = Codec::H264;
	uint8_t channels = 2; // Opus uniquement
};

namespace ts {
constexpr size_t kPacket = 188;
constexpr size_t kPacketsPerMessage = 7;
constexpr size_t kMessage = kPacket * kPacketsPerMessage; // 1316 octets par message SRT

constexpr uint16_t kPidPat = 0x0000;
constexpr uint16_t kPidPmt = 0x1000;
constexpr uint16_t kPidPcr = 0x01FF;
constexpr uint16_t kPidMeta = 0x0300;
constexpr uint16_t kPidVideo0 = 0x0100;
constexpr uint16_t kPidAudio0 = 0x0200;

// Le PCR retarde de 60 ms sur l'heure d'envoi : les PTS (heure de capture) restent devant
// lui, comme l'attendent les lecteurs classiques.
constexpr uint64_t kPcrDelayNs = 60'000'000;
} // namespace ts

uint32_t crc32_mpeg(const uint8_t *data, size_t size);

class TsMuxer {
public:
	using Sink = std::function<void(const uint8_t *data, size_t size)>;

	explicit TsMuxer(Sink sink);

	void set_tracks(const std::vector<Track> &tracks);

	// Chaque appel écrit un bloc complet puis l'envoie. Les appels peuvent venir de
	// plusieurs fils (vidéo, son) : ils sont sérialisés.
	void write_tables();
	void write_pcr(uint64_t now_ns);
	void write_video(uint16_t pid, uint64_t pts_ns, const uint8_t *data, size_t size, bool keyframe);
	void write_audio(uint16_t pid, uint64_t pts_ns, const uint8_t *data, size_t size);
	void write_meta(const std::string &text);

private:
	void write_pes(uint16_t pid, uint8_t stream_id, uint64_t pts_ns, std::span<const uint8_t> prefix,
		       std::span<const uint8_t> payload, bool random_access, bool mark_end);
	void write_section(uint16_t pid, const std::vector<uint8_t> &section);
	void emit_packet(uint16_t pid, bool pusi, uint8_t af_flags, const uint8_t *priv, size_t priv_size,
			 const uint8_t *payload, size_t payload_size);
	void push_packet(const uint8_t *packet);
	void flush();

	Sink sink_;
	std::mutex mutex_;
	std::vector<Track> tracks_;
	std::array<uint8_t, 0x2000> cc_{};
	std::array<uint8_t, ts::kMessage> msg_{};
	size_t msg_size_ = 0;
	std::vector<uint8_t> pes_;
	uint8_t pmt_version_ = 0;
};

struct TsStream {
	uint16_t pid = 0;
	uint8_t stream_type = 0;
	Codec codec = Codec::Meta;
};

struct TsPes {
	uint16_t pid = 0;
	bool has_pts = false;
	uint64_t pts90k = 0; // 33 bits
	const uint8_t *data = nullptr;
	size_t size = 0;
	bool random_access = false;
	bool corrupt = false; // paquet perdu pendant ce PES
};

class TsDemuxer {
public:
	std::function<void(const std::vector<TsStream> &)> on_streams;
	std::function<void(const TsPes &)> on_pes;

	// Reçoit un message (plusieurs paquets de 188 octets).
	void push(const uint8_t *data, size_t size);
	void reset();

private:
	struct PidState {
		bool active = false;
		bool started = false;
		bool corrupt = false;
		bool random_access = false;
		int last_cc = -1;
		size_t expected = 0; // longueur annoncée du PES, 0 = inconnue
		std::vector<uint8_t> buf;
	};

	void packet(const uint8_t *p);
	void section_data(uint16_t pid, bool pusi, const uint8_t *data, size_t size);
	void section(uint16_t pid, const uint8_t *s, size_t size);
	void finish(uint16_t pid, PidState &st);

	std::vector<PidState> pids_ = std::vector<PidState>(0x2000);
	uint16_t pmt_pid_ = 0;
	uint16_t section_pid_ = 0xFFFF;
	std::vector<uint8_t> section_buf_;
	std::vector<TsStream> streams_;
};

// Découpe la charge utile d'un PES Opus (en-têtes de contrôle TS) en paquets Opus.
bool split_opus_pes(const uint8_t *data, size_t size, std::vector<std::span<const uint8_t>> &packets);

} // namespace flash
