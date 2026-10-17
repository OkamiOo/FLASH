#pragma once

#include "flash/video.h"

#include <d3d11_1.h>

#include <array>
#include <unordered_map>

namespace flash {

// Conversion BGRA (taille quelconque) vers NV12 BT.709 à la taille d'encodage, par le
// processeur vidéo du GPU : mise à l'échelle comprise, bandes noires si les proportions
// diffèrent. Indépendant du fabricant de la carte.
class Nv12Converter {
public:
	bool init(ID3D11Device *device, uint32_t out_w, uint32_t out_h);
	// Rend la prochaine texture NV12 de sortie (anneau de 3), ou nullptr en cas d'échec.
	ID3D11Texture2D *convert(ID3D11Texture2D *bgra);

private:
	bool configure(uint32_t in_w, uint32_t in_h);

	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11VideoDevice> vdev_;
	ComPtr<ID3D11VideoContext1> vctx_;
	ComPtr<ID3D11VideoProcessorEnumerator> enum_;
	ComPtr<ID3D11VideoProcessor> proc_;
	uint32_t out_w_ = 0, out_h_ = 0;
	uint32_t in_w_ = 0, in_h_ = 0;
	std::array<ComPtr<ID3D11Texture2D>, 3> out_;
	std::array<ComPtr<ID3D11VideoProcessorOutputView>, 3> out_views_;
	std::unordered_map<ID3D11Texture2D *, ComPtr<ID3D11VideoProcessorInputView>> in_views_;
	uint32_t index_ = 0;
};

} // namespace flash
