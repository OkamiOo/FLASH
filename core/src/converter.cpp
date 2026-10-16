#include "converter.h"
#include "flash/inputs.h"
#include "flash/log.h"

namespace flash {

bool Nv12Converter::init(ID3D11Device *device, uint32_t out_w, uint32_t out_h)
{
	device_ = device;
	out_w_ = out_w;
	out_h_ = out_h;
	ComPtr<ID3D11DeviceContext> ctx;
	device->GetImmediateContext(&ctx);
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&vdev_))) || FAILED(ctx.As(&vctx_))) {
		FLOG_E("Processeur vidéo D3D11 indisponible");
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = out_w;
	desc.Height = out_h;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_NV12;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	for (auto &t : out_) {
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &t)))
			return false;
	}
	return true;
}

bool Nv12Converter::configure(uint32_t in_w, uint32_t in_h)
{
	proc_.Reset();
	enum_.Reset();
	in_views_.clear();
	for (auto &v : out_views_)
		v.Reset();

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputWidth = in_w;
	content.InputHeight = in_h;
	content.OutputWidth = out_w_;
	content.OutputHeight = out_h_;
	content.InputFrameRate = {60, 1};
	content.OutputFrameRate = {60, 1};
	content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
	if (FAILED(vdev_->CreateVideoProcessorEnumerator(&content, &enum_)) ||
	    FAILED(vdev_->CreateVideoProcessor(enum_.Get(), 0, &proc_))) {
		FLOG_E("Création du processeur vidéo impossible (%ux%u vers %ux%u)", in_w, in_h, out_w_, out_h_);
		return false;
	}

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
	ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	for (size_t i = 0; i < out_.size(); i++) {
		if (FAILED(vdev_->CreateVideoProcessorOutputView(out_[i].Get(), enum_.Get(), &ovd, &out_views_[i])))
			return false;
	}

	// Écran ou fenêtre en RVB plein, vers du YUV BT.709 « studio » comme le veut l'encodeur
	vctx_->VideoProcessorSetStreamColorSpace1(proc_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	vctx_->VideoProcessorSetOutputColorSpace1(proc_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
	vctx_->VideoProcessorSetStreamAutoProcessingMode(proc_.Get(), 0, FALSE);

	// Image centrée, proportions gardées, fond noir
	uint32_t w, h;
	fit_size(in_w, in_h, out_w_, out_h_, w, h);
	const RECT src = {0, 0, (LONG)in_w, (LONG)in_h};
	const RECT dst = {(LONG)(out_w_ - w) / 2, (LONG)(out_h_ - h) / 2, (LONG)((out_w_ + w) / 2), (LONG)((out_h_ + h) / 2)};
	vctx_->VideoProcessorSetStreamSourceRect(proc_.Get(), 0, TRUE, &src);
	vctx_->VideoProcessorSetStreamDestRect(proc_.Get(), 0, TRUE, &dst);
	D3D11_VIDEO_COLOR black{};
	black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
	vctx_->VideoProcessorSetOutputBackgroundColor(proc_.Get(), TRUE, &black);

	in_w_ = in_w;
	in_h_ = in_h;
	return true;
}

ID3D11Texture2D *Nv12Converter::convert(ID3D11Texture2D *bgra)
{
	D3D11_TEXTURE2D_DESC d;
	bgra->GetDesc(&d);
	if ((d.Width != in_w_ || d.Height != in_h_ || !proc_) && !configure(d.Width, d.Height))
		return nullptr;

	auto it = in_views_.find(bgra);
	if (it == in_views_.end()) {
		if (in_views_.size() > 8)
			in_views_.clear(); // les textures de capture changent quand la taille change
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
		ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11VideoProcessorInputView> view;
		if (FAILED(vdev_->CreateVideoProcessorInputView(bgra, enum_.Get(), &ivd, &view)))
			return nullptr;
		it = in_views_.emplace(bgra, view).first;
	}

	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.pInputSurface = it->second.Get();
	const uint32_t i = index_++ % (uint32_t)out_.size();
	if (FAILED(vctx_->VideoProcessorBlt(proc_.Get(), out_views_[i].Get(), 0, 1, &stream)))
		return nullptr;
	return out_[i].Get();
}

} // namespace flash
