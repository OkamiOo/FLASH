#pragma once

#include "flash/video.h"

#include <d3d11_1.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// Aperçus pour l'interface : petites vignettes JPEG réduites par le processeur vidéo du GPU,
// captures ponctuelles d'écrans et de fenêtres, icônes en PNG.

namespace flash {

class Thumbnailer {
public:
	explicit Thumbnailer(ComPtr<ID3D11Device> device);
	~Thumbnailer();

	// Image NV12 ou BGRA de taille quelconque, ramenée dans w×h (proportions gardées, bandes
	// noires), en JPEG.
	bool jpeg(ID3D11Texture2D *src, uint32_t w, uint32_t h, std::vector<uint8_t> &out);

private:
	struct Processor;
	Processor *processor(uint32_t in_w, uint32_t in_h, DXGI_FORMAT format, uint32_t out_w, uint32_t out_h);

	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> ctx_;
	ComPtr<ID3D11VideoDevice> vdev_;
	ComPtr<ID3D11VideoContext1> vctx_;
	std::map<std::tuple<uint32_t, uint32_t, int, uint32_t, uint32_t>, std::unique_ptr<Processor>> processors_;
	std::map<std::pair<uint32_t, uint32_t>, ComPtr<ID3D11Texture2D>> copies_; // NV12 lisibles par le processeur vidéo
};

// Une image de l'écran (numéro à partir de 1) ou de la fenêtre (exécutable ou morceau de
// titre), prise une fois, en BGRA. nullptr si elle est introuvable ou réduite.
ComPtr<ID3D11Texture2D> snapshot(ID3D11Device *device, int screen, const std::string &window);

bool encode_png(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, std::vector<uint8_t> &out);
bool encode_jpeg(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, std::vector<uint8_t> &out);
std::string base64(const std::vector<uint8_t> &data);

} // namespace flash
