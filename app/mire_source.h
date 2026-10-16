#pragma once

#include <flash/video.h>

#include <array>
#include <vector>

// Génère les images de la mire directement en NV12 sur le GPU de l'encodeur.
class MireSource {
public:
	bool init(ID3D11Device *device);
	// Dessine l'image n et rend la texture prête à encoder.
	ID3D11Texture2D *render(uint32_t frame, uint64_t capture_ns);

private:
	flash::ComPtr<ID3D11Device> device_;
	flash::ComPtr<ID3D11DeviceContext> context_;
	std::array<flash::ComPtr<ID3D11Texture2D>, 2> staging_;
	std::array<flash::ComPtr<ID3D11Texture2D>, 3> frames_;
	std::vector<uint8_t> background_; // une ligne de dégradé
	uint32_t index_ = 0;
};
