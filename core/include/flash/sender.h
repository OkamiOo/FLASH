#pragma once

#include "flash/srt_link.h"
#include "flash/ts.h"
#include "flash/video.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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

struct SenderOptions {
	SrtOptions srt;
	uint32_t fps = 60;
	uint32_t video_kbps = 25000;
	Codec codec = Codec::H264;
	std::string encoder = "auto"; // "auto", "nvenc", "mf" ou "logiciel"
	int audio_bps = 192000;
	std::string host_name;
};

struct TrackStats {
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
// demande n'est pas encodée.
class Sender {
public:
	Sender();
	~Sender();

	bool start(const SenderOptions &options, ComPtr<ID3D11Device> device, std::vector<std::unique_ptr<VideoInput>> video,
		   std::vector<std::unique_ptr<AudioInput>> audio);
	void stop();

	size_t client_count() const;
	std::vector<TrackStats> stats(); // remet les compteurs à zéro
	// Liste JSON des pistes (pid, type, nom), pour les métadonnées et l'annonce de découverte
	std::string tracks_json() const;

private:
	struct VideoTrack;
	struct AudioTrack;

	void on_video(VideoTrack &t, ID3D11Texture2D *nv12, uint64_t capture_ns);
	void encode_video(VideoTrack &t, ID3D11Texture2D *nv12, uint64_t capture_ns, bool repeat = false);
	void on_audio(AudioTrack &t, const float *samples, uint32_t frames, uint64_t first_ns);
	void on_message(int client, const uint8_t *data, size_t size);
	void housekeeping();
	std::string metadata() const;

	SenderOptions options_;
	ComPtr<ID3D11Device> device_;
	SrtServer server_;
	TsMuxer mux_;
	std::vector<std::unique_ptr<VideoTrack>> video_;
	std::vector<std::unique_ptr<AudioTrack>> audio_;
	std::atomic<bool> running_{false};
	std::thread housekeeping_;
};

} // namespace flash
