#include "flash/preview.h"
#include "flash/log.h"

#include <wincodec.h>

#include <algorithm>
#include <cstring>

namespace flash {

namespace {

// Encodage par WIC (Windows Imaging Component), en mémoire
bool wic_encode(const GUID &container, const GUID &pixel_format, const uint8_t *pixels, uint32_t w, uint32_t h,
		uint32_t stride, float quality, std::vector<uint8_t> &out)
{
	ComPtr<IWICImagingFactory> factory;
	ComPtr<IStream> stream;
	ComPtr<IWICBitmapEncoder> encoder;
	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> props;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
	    FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(factory->CreateEncoder(container, nullptr, &encoder)) ||
	    FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &props)))
		return false;
	if (quality > 0 && props) {
		PROPBAG2 option{};
		option.pstrName = (LPOLESTR)L"ImageQuality";
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_R4;
		v.fltVal = quality;
		props->Write(1, &option, &v);
	}
	WICPixelFormatGUID format = pixel_format;
	if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
	    format != pixel_format || FAILED(frame->WritePixels(h, stride, stride * h, (BYTE *)pixels)) ||
	    FAILED(frame->Commit()) || FAILED(encoder->Commit()))
		return false;

	STATSTG st{};
	HGLOBAL global = nullptr;
	if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &global)))
		return false;
	const uint8_t *p = (const uint8_t *)GlobalLock(global);
	if (!p)
		return false;
	out.assign(p, p + st.cbSize.LowPart);
	GlobalUnlock(global);
	return true;
}

RECT fit(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h)
{
	const double scale = std::min((double)w / std::max<uint32_t>(sw, 1), (double)h / std::max<uint32_t>(sh, 1));
	const LONG dw = (LONG)(sw * scale), dh = (LONG)(sh * scale);
	const LONG x = ((LONG)w - dw) / 2, y = ((LONG)h - dh) / 2;
	return {x, y, x + dw, y + dh};
}

} // namespace

struct Thumbnailer::Processor {
	ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
	ComPtr<ID3D11VideoProcessor> processor;
	ComPtr<ID3D11Texture2D> out, staging;
	ComPtr<ID3D11VideoProcessorOutputView> out_view;
	std::vector<uint8_t> bgr;
};

Thumbnailer::Thumbnailer(ComPtr<ID3D11Device> device) : device_(device)
{
	if (device_) {
		device_->GetImmediateContext(&ctx_);
		device_.As(&vdev_);
		ctx_.As(&vctx_);
	}
}

Thumbnailer::~Thumbnailer() = default;

Thumbnailer::Processor *Thumbnailer::processor(uint32_t in_w, uint32_t in_h, DXGI_FORMAT format, uint32_t out_w,
					       uint32_t out_h)
{
	auto &slot = processors_[{in_w, in_h, (int)format, out_w, out_h}];
	if (slot)
		return slot.get();

	auto p = std::make_unique<Processor>();
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC c{};
	c.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	c.InputWidth = in_w;
	c.InputHeight = in_h;
	c.OutputWidth = out_w;
	c.OutputHeight = out_h;
	c.InputFrameRate = {60, 1};
	c.OutputFrameRate = {60, 1};
	c.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
	if (FAILED(vdev_->CreateVideoProcessorEnumerator(&c, &p->enumerator)) ||
	    FAILED(vdev_->CreateVideoProcessor(p->enumerator.Get(), 0, &p->processor)))
		return nullptr;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = out_w;
	desc.Height = out_h;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (FAILED(device_->CreateTexture2D(&desc, nullptr, &p->out)))
		return nullptr;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(device_->CreateTexture2D(&desc, nullptr, &p->staging)))
		return nullptr;
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
	ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	if (FAILED(vdev_->CreateVideoProcessorOutputView(p->out.Get(), p->enumerator.Get(), &ovd, &p->out_view)))
		return nullptr;

	vctx_->VideoProcessorSetStreamColorSpace1(p->processor.Get(), 0,
						  format == DXGI_FORMAT_NV12 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
									     : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	vctx_->VideoProcessorSetOutputColorSpace1(p->processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	vctx_->VideoProcessorSetStreamAutoProcessingMode(p->processor.Get(), 0, FALSE);
	const RECT src = {0, 0, (LONG)in_w, (LONG)in_h};
	const RECT dst = fit(in_w, in_h, out_w, out_h);
	vctx_->VideoProcessorSetStreamSourceRect(p->processor.Get(), 0, TRUE, &src);
	vctx_->VideoProcessorSetStreamDestRect(p->processor.Get(), 0, TRUE, &dst);
	D3D11_VIDEO_COLOR black{};
	black.RGBA = {0.0f, 0.0f, 0.0f, 1.0f};
	vctx_->VideoProcessorSetOutputBackgroundColor(p->processor.Get(), FALSE, &black);

	slot = std::move(p);
	return slot.get();
}

bool Thumbnailer::jpeg(ID3D11Texture2D *src, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
{
	if (!src || !vdev_ || !vctx_)
		return false;
	D3D11_TEXTURE2D_DESC d;
	src->GetDesc(&d);

	// Une image NV12 de l'envoi passe par une copie que le processeur vidéo accepte en entrée
	ID3D11Texture2D *input = src;
	if (d.Format == DXGI_FORMAT_NV12) {
		auto &copy = copies_[{d.Width, d.Height}];
		if (!copy) {
			D3D11_TEXTURE2D_DESC cd{};
			cd.Width = d.Width;
			cd.Height = d.Height;
			cd.MipLevels = cd.ArraySize = 1;
			cd.Format = DXGI_FORMAT_NV12;
			cd.SampleDesc.Count = 1;
			cd.Usage = D3D11_USAGE_DEFAULT;
			cd.BindFlags = D3D11_BIND_DECODER;
			if (FAILED(device_->CreateTexture2D(&cd, nullptr, &copy)))
				return false;
		}
		ctx_->CopyResource(copy.Get(), src);
		input = copy.Get();
	} else if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
		return false;
	}

	Processor *p = processor(d.Width, d.Height, d.Format, w, h);
	if (!p)
		return false;
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
	ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	ComPtr<ID3D11VideoProcessorInputView> in_view;
	if (FAILED(vdev_->CreateVideoProcessorInputView(input, p->enumerator.Get(), &ivd, &in_view)))
		return false;
	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.pInputSurface = in_view.Get();
	if (FAILED(vctx_->VideoProcessorBlt(p->processor.Get(), p->out_view.Get(), 0, 1, &stream)))
		return false;
	ctx_->CopyResource(p->staging.Get(), p->out.Get());

	D3D11_MAPPED_SUBRESOURCE map;
	if (FAILED(ctx_->Map(p->staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
		return false;
	const uint32_t stride = w * 3;
	p->bgr.resize((size_t)stride * h);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = (const uint8_t *)map.pData + (size_t)y * map.RowPitch;
		uint8_t *o = p->bgr.data() + (size_t)y * stride;
		for (uint32_t x = 0; x < w; x++) {
			o[x * 3] = s[x * 4];
			o[x * 3 + 1] = s[x * 4 + 1];
			o[x * 3 + 2] = s[x * 4 + 2];
		}
	}
	ctx_->Unmap(p->staging.Get(), 0);
	return wic_encode(GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, p->bgr.data(), w, h, stride, 0.75f, out);
}

bool encode_png(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, std::vector<uint8_t> &out)
{
	return wic_encode(GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, bgra, w, h, stride, 0, out);
}

bool encode_jpeg(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, std::vector<uint8_t> &out)
{
	std::vector<uint8_t> bgr((size_t)w * h * 3);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			std::memcpy(&bgr[((size_t)y * w + x) * 3], bgra + (size_t)y * stride + (size_t)x * 4, 3);
	return wic_encode(GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, bgr.data(), w, h, w * 3, 0.75f, out);
}

std::string base64(const std::vector<uint8_t> &data)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		const uint32_t b = (uint32_t)data[i] << 16 | (i + 1 < data.size() ? (uint32_t)data[i + 1] << 8 : 0) |
				   (i + 2 < data.size() ? data[i + 2] : 0);
		out += table[(b >> 18) & 63];
		out += table[(b >> 12) & 63];
		out += i + 1 < data.size() ? table[(b >> 6) & 63] : '=';
		out += i + 2 < data.size() ? table[b & 63] : '=';
	}
	return out;
}

} // namespace flash
