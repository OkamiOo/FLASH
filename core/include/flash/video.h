#pragma once

#include "flash/ts.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flash {

using Microsoft::WRL::ComPtr;

// Périphérique D3D11 sur le GPU qui encode : la carte NVIDIA si elle existe, sinon celle
// par défaut. Protégé pour un usage depuis plusieurs fils.
ComPtr<ID3D11Device> create_d3d11_device(std::string *adapter_name = nullptr);

struct VideoEncoderConfig {
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps_num = 60;
	uint32_t fps_den = 1;
	uint32_t bitrate_kbps = 25000;
	Codec codec = Codec::H264;
};

struct EncodedFrame {
	std::vector<uint8_t> data; // H.264, HEVC : Annex B, AUD en tête ; AV1 : unité temporelle (OBU avec taille)
	bool keyframe = false;
};

class VideoEncoder {
public:
	virtual ~VideoEncoder() = default;

	// Encode une texture NV12 de la taille configurée. Bloque jusqu'à la fin de l'encodage.
	virtual bool encode(ID3D11Texture2D *nv12, bool force_idr, EncodedFrame &out) = 0;
	virtual const char *name() const = 0;
};

std::unique_ptr<VideoEncoder> create_nvenc_encoder(ID3D11Device *device, const VideoEncoderConfig &config);
std::unique_ptr<VideoEncoder> create_mf_encoder(ID3D11Device *device, const VideoEncoderConfig &config);

// NVENC si possible, sinon Media Foundation (matériel de la carte, puis logiciel).
// preference : "auto", "nvenc", "mf" ou "logiciel" (encodeur logiciel de Windows seul).
std::unique_ptr<VideoEncoder> create_video_encoder(ID3D11Device *device, const VideoEncoderConfig &config,
						   const std::string &preference = "auto");

// Formats (bits de flash::formats) que ces encodeurs savent produire sur ce PC
uint8_t nvenc_formats(ID3D11Device *device);
uint8_t video_encoder_formats(ID3D11Device *device, const std::string &preference = "auto");

// Image décodée : une texture NV12 du GPU (parfois une case d'un tableau de textures), valable
// jusqu'au décodage suivant.
struct DecodedFrame {
	ID3D11Texture2D *texture = nullptr;
	UINT subresource = 0;
	RECT visible{}; // partie à montrer (1080 lignes d'une surface de 1088…)
	int64_t time = 0; // horodatage donné avec l'image compressée
};

class VideoDecoder {
public:
	using OnFrame = std::function<void(const DecodedFrame &)>;

	virtual ~VideoDecoder() = default;

	// Une image compressée complète (Annex B). Faux si le décodeur la refuse.
	virtual bool decode(const uint8_t *data, size_t size, int64_t time, const OnFrame &on_frame) = 0;
	virtual const char *name() const = 0;
	virtual bool hardware() const = 0;
};

// Décodeur de Windows (Media Foundation), par la carte graphique si possible, sinon par le
// processeur. HEVC demande l'extension HEVC de Windows, AV1 l'extension AV1.
std::unique_ptr<VideoDecoder> create_mf_decoder(ID3D11Device *device, Codec codec);
// Formats que ce PC sait décoder par Media Foundation ; l'AV1 seulement si la carte le décode.
// installed : formats dont Windows a le décodeur, que la carte suive ou non.
uint8_t mf_decoder_formats(ID3D11Device *device, uint8_t *installed = nullptr);

// La carte de ce périphérique décode-t-elle ce format elle-même (profil DXVA, sortie NV12) ?
// Sans périphérique : la carte par défaut, celle que prend le décodeur d3d11va de FFmpeg.
bool gpu_decodes(ID3D11Device *device, Codec codec);

} // namespace flash
