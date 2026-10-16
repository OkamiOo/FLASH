#include "flash/video.h"
#include "flash/log.h"

#include <ffnvcodec/nvEncodeAPI.h>

#include <cstring>
#include <unordered_map>

namespace flash {

namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
using MaxVersionFn = NVENCSTATUS(NVENCAPI *)(uint32_t *);

class NvencEncoder final : public VideoEncoder {
public:
	~NvencEncoder() override { close(); }

	bool open(ID3D11Device *device, const VideoEncoderConfig &cfg);
	bool encode(ID3D11Texture2D *nv12, bool force_idr, EncodedFrame &out) override;
	const char *name() const override { return "NVENC"; }

private:
	void close();
	bool fail(const char *what, NVENCSTATUS st);
	void *registered(ID3D11Texture2D *tex);

	HMODULE dll_ = nullptr;
	NV_ENCODE_API_FUNCTION_LIST fn_{};
	void *enc_ = nullptr;
	NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
	VideoEncoderConfig cfg_;
	std::unordered_map<ID3D11Texture2D *, NV_ENC_REGISTERED_PTR> resources_;
};

bool NvencEncoder::fail(const char *what, NVENCSTATUS st)
{
	const char *detail = (enc_ && fn_.nvEncGetLastErrorString) ? fn_.nvEncGetLastErrorString(enc_) : "";
	FLOG_E("NVENC : %s a échoué (%d) %s", what, (int)st, detail ? detail : "");
	return false;
}

bool NvencEncoder::open(ID3D11Device *device, const VideoEncoderConfig &cfg)
{
	cfg_ = cfg;
	dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
	if (!dll_)
		return false;

	auto max_version = (MaxVersionFn)GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion");
	auto create = (CreateInstanceFn)GetProcAddress(dll_, "NvEncodeAPICreateInstance");
	if (!max_version || !create)
		return false;

	uint32_t version = 0;
	max_version(&version);
	const uint32_t wanted = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
	if (version < wanted) {
		FLOG_W("Pilote NVIDIA trop ancien pour NVENC %u.%u (il gère %u.%u)", NVENCAPI_MAJOR_VERSION,
		       NVENCAPI_MINOR_VERSION, version >> 4, version & 0x0F);
		return false;
	}

	fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
	if (create(&fn_) != NV_ENC_SUCCESS)
		return false;

	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session = {NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
	session.device = device;
	session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	session.apiVersion = NVENCAPI_VERSION;
	NVENCSTATUS st = fn_.nvEncOpenEncodeSessionEx(&session, &enc_);
	if (st != NV_ENC_SUCCESS) {
		enc_ = nullptr;
		return fail("l'ouverture de session", st);
	}

	const GUID codec = cfg.codec == Codec::HEVC ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
	const GUID preset = NV_ENC_PRESET_P2_GUID;
	const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

	NV_ENC_PRESET_CONFIG preset_cfg = {NV_ENC_PRESET_CONFIG_VER, 0, {NV_ENC_CONFIG_VER}};
	st = fn_.nvEncGetEncodePresetConfigEx(enc_, codec, preset, tuning, &preset_cfg);
	if (st != NV_ENC_SUCCESS)
		return fail("la lecture du preset", st);

	NV_ENC_CONFIG config = preset_cfg.presetCfg;
	config.version = NV_ENC_CONFIG_VER;
	config.gopLength = NVENC_INFINITE_GOPLENGTH;
	config.frameIntervalP = 1; // aucune B-frame

	// CBR avec un tampon d'une seule image : taille d'image régulière, pas d'attente
	const uint32_t bitrate = cfg.bitrate_kbps * 1000;
	NV_ENC_RC_PARAMS &rc = config.rcParams;
	rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	rc.averageBitRate = bitrate;
	rc.maxBitRate = bitrate;
	rc.vbvBufferSize = (uint32_t)((uint64_t)bitrate * cfg.fps_den / cfg.fps_num);
	rc.vbvInitialDelay = rc.vbvBufferSize;
	rc.zeroReorderDelay = 1;
	rc.enableLookahead = 0;

	const uint32_t refresh = cfg.fps_num / cfg.fps_den; // un rafraîchissement complet par seconde
	if (cfg.codec == Codec::HEVC) {
		NV_ENC_CONFIG_HEVC &h = config.encodeCodecConfig.hevcConfig;
		h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
		h.repeatSPSPPS = 1;
		h.outputAUD = 1;
		h.enableIntraRefresh = 1;
		h.intraRefreshPeriod = refresh;
		h.intraRefreshCnt = refresh / 6;
		h.outputRecoveryPointSEI = 1;
		h.hevcVUIParameters.videoSignalTypePresentFlag = 1;
		h.hevcVUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
		h.hevcVUIParameters.colourDescriptionPresentFlag = 1;
		h.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		h.hevcVUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
		h.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
	} else {
		config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
		NV_ENC_CONFIG_H264 &h = config.encodeCodecConfig.h264Config;
		h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
		h.repeatSPSPPS = 1;
		h.outputAUD = 1;
		h.enableIntraRefresh = 1;
		h.intraRefreshPeriod = refresh;
		h.intraRefreshCnt = refresh / 6;
		h.outputRecoveryPointSEI = 1;
		h.h264VUIParameters.videoSignalTypePresentFlag = 1;
		h.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
		h.h264VUIParameters.colourDescriptionPresentFlag = 1;
		h.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		h.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
		h.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
	}

	NV_ENC_INITIALIZE_PARAMS init = {NV_ENC_INITIALIZE_PARAMS_VER};
	init.encodeGUID = codec;
	init.presetGUID = preset;
	init.tuningInfo = tuning;
	init.encodeWidth = init.darWidth = init.maxEncodeWidth = cfg.width;
	init.encodeHeight = init.darHeight = init.maxEncodeHeight = cfg.height;
	init.frameRateNum = cfg.fps_num;
	init.frameRateDen = cfg.fps_den;
	init.enablePTD = 1;
	init.encodeConfig = &config;

	st = fn_.nvEncInitializeEncoder(enc_, &init);
	if (st != NV_ENC_SUCCESS)
		return fail("l'initialisation", st);

	NV_ENC_CREATE_BITSTREAM_BUFFER bs = {NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
	st = fn_.nvEncCreateBitstreamBuffer(enc_, &bs);
	if (st != NV_ENC_SUCCESS)
		return fail("la création du tampon de sortie", st);
	bitstream_ = bs.bitstreamBuffer;

	FLOG_I("NVENC prêt : %s %ux%u à %u/%u i/s, %u kb/s", cfg.codec == Codec::HEVC ? "HEVC" : "H.264", cfg.width,
	       cfg.height, cfg.fps_num, cfg.fps_den, cfg.bitrate_kbps);
	return true;
}

void NvencEncoder::close()
{
	if (enc_) {
		for (auto &[tex, reg] : resources_)
			fn_.nvEncUnregisterResource(enc_, reg);
		resources_.clear();
		if (bitstream_)
			fn_.nvEncDestroyBitstreamBuffer(enc_, bitstream_);
		fn_.nvEncDestroyEncoder(enc_);
		enc_ = nullptr;
		bitstream_ = nullptr;
	}
	if (dll_) {
		FreeLibrary(dll_);
		dll_ = nullptr;
	}
}

void *NvencEncoder::registered(ID3D11Texture2D *tex)
{
	auto it = resources_.find(tex);
	if (it != resources_.end())
		return it->second;

	NV_ENC_REGISTER_RESOURCE reg = {NV_ENC_REGISTER_RESOURCE_VER};
	reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
	reg.resourceToRegister = tex;
	reg.width = cfg_.width;
	reg.height = cfg_.height;
	reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
	reg.bufferUsage = NV_ENC_INPUT_IMAGE;
	const NVENCSTATUS st = fn_.nvEncRegisterResource(enc_, &reg);
	if (st != NV_ENC_SUCCESS) {
		fail("l'enregistrement de la texture", st);
		return nullptr;
	}
	resources_[tex] = reg.registeredResource;
	return reg.registeredResource;
}

bool NvencEncoder::encode(ID3D11Texture2D *nv12, bool force_idr, EncodedFrame &out)
{
	void *reg = registered(nv12);
	if (!reg)
		return false;

	NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER};
	map.registeredResource = reg;
	NVENCSTATUS st = fn_.nvEncMapInputResource(enc_, &map);
	if (st != NV_ENC_SUCCESS)
		return fail("le mappage de l'entrée", st);

	NV_ENC_PIC_PARAMS pic = {NV_ENC_PIC_PARAMS_VER};
	pic.inputBuffer = map.mappedResource;
	pic.bufferFmt = map.mappedBufferFmt;
	pic.inputWidth = cfg_.width;
	pic.inputHeight = cfg_.height;
	pic.outputBitstream = bitstream_;
	pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	if (force_idr)
		pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

	st = fn_.nvEncEncodePicture(enc_, &pic);
	bool ok = st == NV_ENC_SUCCESS;
	if (!ok) {
		fail("l'encodage", st);
	} else {
		NV_ENC_LOCK_BITSTREAM lock = {NV_ENC_LOCK_BITSTREAM_VER};
		lock.outputBitstream = bitstream_;
		st = fn_.nvEncLockBitstream(enc_, &lock);
		if (st == NV_ENC_SUCCESS) {
			const auto *p = (const uint8_t *)lock.bitstreamBufferPtr;
			out.data.assign(p, p + lock.bitstreamSizeInBytes);
			out.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
			fn_.nvEncUnlockBitstream(enc_, bitstream_);
		} else {
			ok = fail("la lecture du flux", st);
		}
	}

	fn_.nvEncUnmapInputResource(enc_, map.mappedResource);
	return ok;
}

} // namespace

std::unique_ptr<VideoEncoder> create_nvenc_encoder(ID3D11Device *device, const VideoEncoderConfig &config)
{
	auto enc = std::make_unique<NvencEncoder>();
	if (!enc->open(device, config))
		return nullptr;
	return enc;
}

} // namespace flash
