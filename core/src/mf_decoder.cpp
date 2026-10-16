// Décodage H.264 et HEVC par Media Foundation : le décodeur fourni par Windows, accéléré par
// la carte graphique (DXVA) quand il accepte nos textures, sinon par le processeur.

#include "flash/inputs.h"
#include "flash/log.h"
#include "flash/video.h"

#include <initguid.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include <cstring>

namespace flash {

namespace {

class MfDecoder final : public VideoDecoder {
public:
	~MfDecoder() override
	{
		if (mft_) {
			mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		}
		mft_.Reset();
		manager_.Reset();
		if (started_)
			MFShutdown();
	}

	bool open(ID3D11Device *device, Codec codec, bool hardware);
	bool decode(const uint8_t *data, size_t size, int64_t time, const OnFrame &on_frame) override;
	const char *name() const override { return name_.c_str(); }
	bool hardware() const override { return manager_ != nullptr; }

private:
	bool choose_output();
	bool drain(const OnFrame &on_frame);
	bool deliver(IMFSample *sample, const OnFrame &on_frame);
	bool upload(IMFMediaBuffer *buffer, int64_t time, const OnFrame &on_frame);

	ComPtr<ID3D11Device> device_;
	ComPtr<IMFTransform> mft_;
	ComPtr<IMFDXGIDeviceManager> manager_;
	bool started_ = false;
	bool provides_samples_ = false;
	DWORD out_size_ = 0;
	uint32_t width_ = 0, height_ = 0;
	RECT visible_{};
	ComPtr<ID3D11Texture2D> staging_, frame_; // décodage par le processeur : l'image remonte au GPU
	std::string name_ = "Media Foundation";
};

bool MfDecoder::open(ID3D11Device *device, Codec codec, bool hardware)
{
	device_ = device;
	if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
		return false;
	started_ = true;

	MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, codec == Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264};
	IMFActivate **acts = nullptr;
	UINT32 count = 0;
	if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
			     MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts,
			     &count)) ||
	    !count)
		return false;
	HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft_));
	wchar_t *friendly = nullptr;
	UINT32 len = 0;
	if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
		name_ = to_utf8(friendly);
		CoTaskMemFree(friendly);
	}
	for (UINT32 i = 0; i < count; i++)
		acts[i]->Release();
	CoTaskMemFree(acts);
	if (FAILED(hr))
		return false;

	// Chaque image sort dès qu'elle est décodée, sans attendre les suivantes
	ComPtr<IMFAttributes> attrs;
	if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
		attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
		UINT32 aware = 0;
		if (hardware && (FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware))
			return false;
	}
	ComPtr<ICodecAPI> api;
	if (SUCCEEDED(mft_.As(&api))) {
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_BOOL;
		v.boolVal = VARIANT_TRUE;
		api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
	}

	if (hardware) {
		UINT token = 0;
		if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(device, token)) ||
		    FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager_.Get()))) {
			manager_.Reset();
			return false;
		}
	}

	ComPtr<IMFMediaType> type;
	MFCreateMediaType(&type);
	type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	type->SetGUID(MF_MT_SUBTYPE, in.guidSubtype);
	type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (FAILED(mft_->SetInputType(0, type.Get(), 0)))
		return false;
	choose_output(); // la vraie taille arrive avec la première image clé

	mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	FLOG_I("Décodeur %s : %s%s", codec == Codec::HEVC ? "HEVC" : "H.264", name_.c_str(),
	       hardware ? " (carte graphique)" : " (processeur)");
	return true;
}

bool MfDecoder::choose_output()
{
	for (DWORD i = 0;; i++) {
		ComPtr<IMFMediaType> t;
		if (FAILED(mft_->GetOutputAvailableType(0, i, &t)))
			return false;
		GUID sub{};
		t->GetGUID(MF_MT_SUBTYPE, &sub);
		if (sub != MFVideoFormat_NV12)
			continue;
		if (FAILED(mft_->SetOutputType(0, t.Get(), 0)))
			return false;

		UINT32 w = 0, h = 0;
		MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
		width_ = w;
		height_ = h;
		visible_ = {0, 0, (LONG)w, (LONG)h};
		MFVideoArea area{};
		if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr)) && area.Area.cx &&
		    area.Area.cy) {
			visible_.left = area.OffsetX.value;
			visible_.top = area.OffsetY.value;
			visible_.right = visible_.left + area.Area.cx;
			visible_.bottom = visible_.top + area.Area.cy;
		}

		MFT_OUTPUT_STREAM_INFO info{};
		mft_->GetOutputStreamInfo(0, &info);
		provides_samples_ =
			(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
		out_size_ = info.cbSize;
		staging_.Reset();
		frame_.Reset();
		return true;
	}
}

bool MfDecoder::decode(const uint8_t *data, size_t size, int64_t time, const OnFrame &on_frame)
{
	ComPtr<IMFMediaBuffer> buf;
	BYTE *p = nullptr;
	if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buf)) || FAILED(buf->Lock(&p, nullptr, nullptr)))
		return false;
	std::memcpy(p, data, size);
	buf->Unlock();
	buf->SetCurrentLength((DWORD)size);
	ComPtr<IMFSample> sample;
	MFCreateSample(&sample);
	sample->AddBuffer(buf.Get());
	sample->SetSampleTime(time);

	HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
	if (hr == MF_E_NOTACCEPTING) {
		if (!drain(on_frame))
			return false;
		hr = mft_->ProcessInput(0, sample.Get(), 0);
	}
	return SUCCEEDED(hr) && drain(on_frame);
}

bool MfDecoder::drain(const OnFrame &on_frame)
{
	for (;;) {
		MFT_OUTPUT_DATA_BUFFER db{};
		ComPtr<IMFSample> own;
		if (!provides_samples_) {
			ComPtr<IMFMediaBuffer> mb;
			MFCreateSample(&own);
			MFCreateMemoryBuffer(out_size_ ? out_size_ : 1920 * 1088 * 3 / 2, &mb);
			own->AddBuffer(mb.Get());
			db.pSample = own.Get();
		}
		DWORD status = 0;
		const HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
		if (db.pEvents)
			db.pEvents->Release();
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
			return true;
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			if (!choose_output())
				return false;
			continue;
		}
		if (FAILED(hr))
			return false;
		if (db.pSample) {
			deliver(db.pSample, on_frame);
			if (provides_samples_)
				db.pSample->Release();
		}
	}
}

bool MfDecoder::deliver(IMFSample *sample, const OnFrame &on_frame)
{
	LONGLONG time = 0;
	sample->GetSampleTime(&time);
	ComPtr<IMFMediaBuffer> buffer;
	if (FAILED(sample->GetBufferByIndex(0, &buffer)))
		return false;

	ComPtr<IMFDXGIBuffer> dxgi;
	if (SUCCEEDED(buffer.As(&dxgi))) {
		ComPtr<ID3D11Texture2D> tex;
		UINT index = 0;
		if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgi->GetSubresourceIndex(&index)))
			return false;
		on_frame({tex.Get(), index, visible_, time});
		return true;
	}
	return upload(buffer.Get(), time, on_frame);
}

// Décodage par le processeur : l'image NV12 passe dans une texture du GPU
bool MfDecoder::upload(IMFMediaBuffer *buffer, int64_t time, const OnFrame &on_frame)
{
	if (!frame_) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width_;
		desc.Height = height_;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_)))
			return false;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		if (FAILED(device_->CreateTexture2D(&desc, nullptr, &frame_)))
			return false;
	}

	BYTE *src = nullptr;
	LONG pitch = 0;
	ComPtr<IMF2DBuffer> b2;
	const bool locked2d = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&src, &pitch));
	if (!locked2d) {
		if (FAILED(buffer->Lock(&src, nullptr, nullptr)))
			return false;
		pitch = (LONG)width_;
	}

	ComPtr<ID3D11DeviceContext> ctx;
	device_->GetImmediateContext(&ctx);
	D3D11_MAPPED_SUBRESOURCE map;
	const bool mapped = SUCCEEDED(ctx->Map(staging_.Get(), 0, D3D11_MAP_WRITE, 0, &map));
	if (mapped) {
		// Plan Y puis plan UV, à la suite, avec le même pas
		for (uint32_t line = 0; line < height_ * 3 / 2; line++)
			std::memcpy((uint8_t *)map.pData + (size_t)line * map.RowPitch, src + (size_t)line * pitch, width_);
		ctx->Unmap(staging_.Get(), 0);
		ctx->CopyResource(frame_.Get(), staging_.Get());
	}
	if (locked2d)
		b2->Unlock2D();
	else
		buffer->Unlock();
	if (mapped)
		on_frame({frame_.Get(), 0, visible_, time});
	return mapped;
}

} // namespace

std::unique_ptr<VideoDecoder> create_mf_decoder(ID3D11Device *device, Codec codec)
{
	for (bool hardware : {true, false}) {
		auto dec = std::make_unique<MfDecoder>();
		if (dec->open(device, codec, hardware))
			return dec;
	}
	return nullptr;
}

} // namespace flash
