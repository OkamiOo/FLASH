#pragma once

#include "flash/ts.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
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
	std::vector<uint8_t> data; // Annex B, AUD en tête
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

} // namespace flash
