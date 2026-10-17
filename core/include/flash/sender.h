#pragma once

#include "flash/srt_link.h"
#include "flash/ts.h"
#include "flash/video.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace flash {

// Une entrée vidéo livre des images NV12 à sa taille d'encodage, horodatées à la capture.
class VideoInput {
public:
	using OnFrame = std::function<void(ID3D11Texture2D *nv12, uint64_t capture_ns)>;

	virtual ~VideoInput() = default;
	virtual bool start(ID3D11Device *device, OnFrame on_frame) = 0;
	virtual void stop() = 0;
	virtual uint32_t width() const = 0;
	virtual uint32_t height() const = 0;
	virtual std::string name() const = 0;
};

// Une entrée son livre du 48 kHz stéréo en flottants entrelacés, horodaté au premier
// échantillon.
class AudioInput {
public:
	using OnSamples = std::function<void(const float *samples, uint32_t frames, uint64_t first_ns)>;

	virtual ~AudioInput() = default;
	virtual bool start(OnSamples on_samples) = 0;
	virtual void stop() = 0;
	virtual std::string name() const = 0;
};

struct VideoOptions {
	uint32_t fps = 60;
	uint32_t kbps = 25000;
	Codec codec = Codec::H264;    // format voulu ; envoyé seulement si l'encodeur et tous les récepteurs le gèrent
	std::string encoder = "auto"; // "auto", "nvenc", "mf" ou "logiciel"
};

struct SenderOptions {
	SrtOptions srt;
	VideoOptions video;
	int audio_bps = 192000;
	std::string host_name;
};

struct TrackStats {
	std::string key;
	std::string name;
	bool video = false;
	uint64_t captured = 0; // images livrées par la capture
	uint64_t frames = 0;   // images (ou trames son) encodées
	uint64_t keyframes = 0;
	uint64_t bytes = 0;
	double encode_ms_avg = 0;
	double encode_ms_max = 0;
	bool active = false; // au moins un récepteur la demande
};

// L'envoi : chaque entrée a sa piste et son encodeur, toutes partagent le même flux
// MPEG-TS, la même horloge et les mêmes connexions SRT. Une piste qu'aucun récepteur ne
// demande n'est pas encodée. Les pistes s'ajoutent et se retirent en marche, sans couper
// les autres : les récepteurs voient une nouvelle table de pistes.
class Sender {
public:
	Sender();
	~Sender();

	bool start(const SenderOptions &options, ComPtr<ID3D11Device> device);
	void stop();

	// key : la source (« ecran|1 », « appli|discord.exe »…), unique
	bool add_video(const std::string &key, std::unique_ptr<VideoInput> input);
	bool add_audio(const std::string &key, std::unique_ptr<AudioInput> input, float gain = 1.0f);
	void remove(const std::string &key);
	bool has(const std::string &key) const;
	void set_gain(const std::string &key, float gain);
	// Débit, format ou encodeur : les encodeurs vidéo sont refaits, les captures continuent
	void set_video_options(const VideoOptions &video);

	// Formats vidéo : ceux que l'encodeur de ce PC sait produire, ceux qui sont possibles
	// (encodeur et tous les récepteurs connectés), et celui qui part. Le format voulu part s'il
	// est possible ; sinon H.264, et le format voulu revient dès qu'il le redevient.
	uint8_t encoder_formats() const { return encoder_formats_.load(); }
	uint8_t possible_formats() const;
	Codec video_codec() const { return codec_.load(); }

	size_t client_count() const;
	std::vector<TrackStats> stats(); // remet les compteurs à zéro
	// Liste JSON des pistes (pid, type, nom), pour les métadonnées et l'annonce de découverte
	std::string tracks_json() const;

	// Crête du son envoyé (0 à 1) depuis le dernier appel, par piste son
	std::map<std::string, float> levels();
	// Dernière image envoyée d'une piste vidéo (NV12), pour un aperçu
	ComPtr<ID3D11Texture2D> last_frame(const std::string &key) const;
	// Latence estimée de la capture à l'affichage chez le récepteur le plus lent, en ms
	// (0 si aucun récepteur ne l'a donnée récemment)
	double receiver_latency_ms();

private:
	struct VideoTrack;
	struct AudioTrack;

	void on_video(VideoTrack &t, ID3D11Texture2D *nv12, uint64_t capture_ns);
	void encode_video(VideoTrack &t, ID3D11Texture2D *nv12, uint64_t capture_ns, bool repeat = false);
	bool open_encoder(VideoTrack &t);
	Codec choose_codec() const; // tracks_ tenu
	void update_codec();
	void reopen_encoders(); // tracks_ tenu en écriture
	void on_audio(AudioTrack &t, const float *samples, uint32_t frames, uint64_t first_ns);
	void on_message(int client, const uint8_t *data, size_t size);
	void publish_tracks(); // tracks_ tenu en écriture
	void housekeeping();
	std::string metadata() const; // tracks_ tenu
	std::string tracks_json_unlocked() const;
	uint16_t free_pid(uint16_t base) const;

	SenderOptions options_;
	ComPtr<ID3D11Device> device_;
	SrtServer server_;
	TsMuxer mux_;

	// Lecteurs : fil de service, voie de retour, statistiques. Écrivains : ajout et retrait.
	mutable std::shared_mutex tracks_;
	std::vector<std::unique_ptr<VideoTrack>> video_;
	std::vector<std::unique_ptr<AudioTrack>> audio_;
	std::string meta_;

	std::mutex reports_mutex_;
	std::map<int, std::pair<uint64_t, double>> reports_; // client -> (heure, ms de l'arrivée à l'affichage)
	std::atomic<uint64_t> send_lag_ns_{0}; // de la capture à l'envoi, moyenne glissante
	std::atomic<uint8_t> encoder_formats_{0};
	std::atomic<Codec> codec_{Codec::H264}; // format envoyé

	std::atomic<bool> running_{false};
	std::thread housekeeping_;
};

} // namespace flash
