// Repli d'encodage par Media Foundation : l'encodeur matériel que le pilote de la carte
// fournit à Windows (AMD, Intel, NVIDIA), sinon l'encodeur logiciel de Windows.

#include "flash/clock.h"
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
#include <dxgi.h>

#include <atomic>

namespace flash {

namespace {

constexpr uint64_t kOutputTimeoutNs = 200'000'000ULL;

void set_codec_value(ICodecAPI *api, const GUID &key, ULONG value)
{
	VARIANT v;
	VariantInit(&v);
	v.vt = VT_UI4;
	v.ulVal = value;
	api->SetValue(&key, &v);
}

class MfEncoder final : public VideoEncoder {
public:
	~MfEncoder() override
	{
		if (mft_) {
			mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		}
		mft_.Reset();
		events_.Reset();
		codec_.Reset();
		manager_.Reset();
		if (started_)
			MFShutdown();
	}

	bool open(ID3D11Device *device, const VideoEncoderConfig &cfg, bool hardware);
	bool encode(ID3D11Texture2D *nv12, bool force_idr, EncodedFrame &out) override;
	const char *name() const override { return name_.c_str(); }

private:
	bool next_event(MediaEventType &type);
	bool pull_output(EncodedFrame &out);
	bool make_sample(ID3D11Texture2D *nv12, ComPtr<IMFSample> &sample);

	VideoEncoderConfig cfg_;
	ComPtr<ID3D11Device> device_;
	ComPtr<IMFTransform> mft_;
	ComPtr<IMFMediaEventGenerator> events_;
	ComPtr<ICodecAPI> codec_;
	ComPtr<IMFDXGIDeviceManager> manager_;
	ComPtr<ID3D11Texture2D> staging_; // encodeur logiciel : l'image passe par la mémoire
	bool started_ = false;
	bool async_ = false;
	bool provides_samples_ = false;
	DWORD out_size_ = 0;
	int need_input_ = 0;
	int64_t frame_ = 0;
	std::string name_ = "Media Foundation";
};

bool MfEncoder::open(ID3D11Device *device, const VideoEncoderConfig &cfg, bool hardware)
{
	cfg_ = cfg;
	device_ = device;
	if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
		return false;
	started_ = true;

	MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
	MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, cfg.codec == Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264};

	// Encodeur matériel de la carte qui porte notre périphérique D3D11
	ComPtr<IMFAttributes> filter;
	if (hardware) {
		ComPtr<IDXGIDevice> dxgi;
		ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc{};
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
		    SUCCEEDED(adapter->GetDesc(&desc))) {
			MFCreateAttributes(&filter, 1);
			filter->SetBlob(MFT_ENUM_ADAPTER_LUID, (const UINT8 *)&desc.AdapterLuid, sizeof(desc.AdapterLuid));
		}
	}
	IMFActivate **acts = nullptr;
	UINT32 count = 0;
	const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
	if (FAILED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, filter.Get(), &acts, &count)) || !count)
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

	ComPtr<IMFAttributes> attrs;
	if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
		UINT32 is_async = 0;
		if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
			attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
			async_ = SUCCEEDED(mft_.As(&events_));
		}
		attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
	}

	if (hardware) {
		UINT token = 0;
		if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(device, token)) ||
		    FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager_.Get()))) {
			FLOG_W("%s : textures du GPU refusées", name_.c_str());
			return false;
		}
	}

	// Faible latence, débit constant, aucune B-frame, image clé toutes les 2 s
	if (SUCCEEDED(mft_.As(&codec_))) {
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_BOOL;
		v.boolVal = VARIANT_TRUE;
		codec_->SetValue(&CODECAPI_AVLowLatencyMode, &v);
		set_codec_value(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
		set_codec_value(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, cfg.bitrate_kbps * 1000);
		set_codec_value(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
		set_codec_value(codec_.Get(), CODECAPI_AVEncMPVGOPSize, cfg.fps_num / cfg.fps_den * 2);
	}

	ComPtr<IMFMediaType> type;
	MFCreateMediaType(&type);
	type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	type->SetGUID(MF_MT_SUBTYPE, out.guidSubtype);
	type->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrate_kbps * 1000);
	MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
	MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, cfg.fps_num, cfg.fps_den);
	MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (cfg.codec == Codec::H264)
		type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
	if (FAILED(mft_->SetOutputType(0, type.Get(), 0)))
		return false;

	ComPtr<IMFMediaType> itype;
	MFCreateMediaType(&itype);
	itype->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	itype->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	MFSetAttributeSize(itype.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
	MFSetAttributeRatio(itype.Get(), MF_MT_FRAME_RATE, cfg.fps_num, cfg.fps_den);
	itype->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (FAILED(mft_->SetInputType(0, itype.Get(), 0)))
		return false;

	MFT_OUTPUT_STREAM_INFO info{};
	mft_->GetOutputStreamInfo(0, &info);
	provides_samples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
	out_size_ = info.cbSize ? info.cbSize : cfg.width * cfg.height * 2;

	if (!hardware) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = cfg.width;
		desc.Height = cfg.height;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging_)))
			return false;
	}

	mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	FLOG_I("%s prêt : %s %ux%u, %u kb/s%s", name_.c_str(), cfg.codec == Codec::HEVC ? "HEVC" : "H.264", cfg.width,
	       cfg.height, cfg.bitrate_kbps, hardware ? "" : " (encodage par le processeur)");
	return true;
}

// Prochain événement de l'encodeur asynchrone, sans bloquer plus de 200 ms
bool MfEncoder::next_event(MediaEventType &type)
{
	const uint64_t deadline = now_ns() + kOutputTimeoutNs;
	for (;;) {
		ComPtr<IMFMediaEvent> ev;
		const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
		if (SUCCEEDED(hr)) {
			ev->GetType(&type);
			return true;
		}
		if (hr != MF_E_NO_EVENTS_AVAILABLE || now_ns() > deadline)
			return false;
		sleep_until_ns(now_ns() + 200'000);
	}
}

bool MfEncoder::make_sample(ID3D11Texture2D *nv12, ComPtr<IMFSample> &sample)
{
	ComPtr<IMFMediaBuffer> buffer;
	if (manager_) {
		if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer)))
			return false;
	} else {
		ComPtr<ID3D11DeviceContext> ctx;
		device_->GetImmediateContext(&ctx);
		ctx->CopyResource(staging_.Get(), nv12);
		D3D11_MAPPED_SUBRESOURCE map;
		if (FAILED(ctx->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map)))
			return false;
		const DWORD size = cfg_.width * cfg_.height * 3 / 2;
		BYTE *dst = nullptr;
		if (SUCCEEDED(MFCreateMemoryBuffer(size, &buffer)) && SUCCEEDED(buffer->Lock(&dst, nullptr, nullptr))) {
			const uint8_t *src = (const uint8_t *)map.pData;
			for (uint32_t line = 0; line < cfg_.height * 3 / 2; line++)
				memcpy(dst + (size_t)line * cfg_.width, src + (size_t)line * map.RowPitch, cfg_.width);
			buffer->Unlock();
			buffer->SetCurrentLength(size);
		}
		ctx->Unmap(staging_.Get(), 0);
		if (!dst)
			return false;
	}
	MFCreateSample(&sample);
	sample->AddBuffer(buffer.Get());
	const LONGLONG duration = 10'000'000LL * cfg_.fps_den / cfg_.fps_num;
	sample->SetSampleTime(frame_++ * duration);
	sample->SetSampleDuration(duration);
	return true;
}

bool MfEncoder::pull_output(EncodedFrame &out)
{
	MFT_OUTPUT_DATA_BUFFER db{};
	ComPtr<IMFSample> own;
	if (!provides_samples_) {
		ComPtr<IMFMediaBuffer> mb;
		MFCreateSample(&own);
		MFCreateMemoryBuffer(out_size_, &mb);
		own->AddBuffer(mb.Get());
		db.pSample = own.Get();
	}
	DWORD status = 0;
	const HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
	if (db.pEvents)
		db.pEvents->Release();
	if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
		ComPtr<IMFMediaType> type;
		if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &type)))
			mft_->SetOutputType(0, type.Get(), 0);
		return false;
	}
	if (FAILED(hr) || !db.pSample)
		return false;

	ComPtr<IMFMediaBuffer> buf;
	BYTE *p = nullptr;
	DWORD len = 0;
	bool ok = SUCCEEDED(db.pSample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&p, nullptr, &len));
	if (ok) {
		out.data.assign(p, p + len);
		buf->Unlock();
		UINT32 clean = 0;
		db.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
		out.keyframe = clean != 0;
	}
	if (provides_samples_)
		db.pSample->Release();
	return ok;
}

bool MfEncoder::encode(ID3D11Texture2D *nv12, bool force_idr, EncodedFrame &out)
{
	if (force_idr && codec_)
		set_codec_value(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

	ComPtr<IMFSample> sample;
	if (!make_sample(nv12, sample))
		return false;

	if (!async_) {
		if (FAILED(mft_->ProcessInput(0, sample.Get(), 0)))
			return false;
		return pull_output(out);
	}

	// Encodeur asynchrone : il réclame chaque image, puis annonce le résultat
	MediaEventType type = MEUnknown;
	bool got = false;
	while (need_input_ == 0) {
		if (!next_event(type))
			return false;
		if (type == METransformNeedInput)
			need_input_++;
		else if (type == METransformHaveOutput)
			got = pull_output(out) || got;
	}
	need_input_--;
	if (FAILED(mft_->ProcessInput(0, sample.Get(), 0)))
		return false;
	while (!got) {
		if (!next_event(type))
			return false;
		if (type == METransformNeedInput)
			need_input_++;
		else if (type == METransformHaveOutput)
			got = pull_output(out);
	}
	return true;
}

} // namespace

std::unique_ptr<VideoEncoder> create_mf_encoder(ID3D11Device *device, const VideoEncoderConfig &config)
{
	for (bool hardware : {true, false}) {
		auto enc = std::make_unique<MfEncoder>();
		if (enc->open(device, config, hardware))
			return enc;
	}
	return nullptr;
}

std::unique_ptr<VideoEncoder> create_video_encoder(ID3D11Device *device, const VideoEncoderConfig &config,
						   const std::string &preference)
{
	if (preference == "logiciel") {
		auto enc = std::make_unique<MfEncoder>();
		return enc->open(device, config, false) ? std::move(enc) : nullptr;
	}
	if (preference != "mf") {
		if (auto enc = create_nvenc_encoder(device, config))
			return enc;
		if (preference == "nvenc")
			return nullptr;
	}
	return create_mf_encoder(device, config);
}

} // namespace flash
