#include "flash/video.h"
#include "flash/log.h"

#include <dxgi1_6.h>
#include <d3d10.h>

namespace flash {

ComPtr<ID3D11Device> create_d3d11_device(std::string *adapter_name)
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return nullptr;

	// NVENC veut le GPU NVIDIA ; à défaut, le premier adaptateur matériel
	ComPtr<IDXGIAdapter1> chosen;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			continue;
		if (!chosen || desc.VendorId == 0x10DE) {
			chosen = adapter;
			if (desc.VendorId == 0x10DE)
				break;
		}
	}
	if (!chosen)
		return nullptr;

	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	ComPtr<ID3D11Device> device;
	const HRESULT hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
					     D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
					     2, D3D11_SDK_VERSION, &device, nullptr, nullptr);
	if (FAILED(hr)) {
		FLOG_E("Création du périphérique D3D11 impossible (0x%08lx)", (unsigned long)hr);
		return nullptr;
	}

	ComPtr<ID3D10Multithread> mt;
	if (SUCCEEDED(device.As(&mt)))
		mt->SetMultithreadProtected(TRUE);

	DXGI_ADAPTER_DESC1 desc;
	chosen->GetDesc1(&desc);
	char name[128];
	WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
	if (adapter_name)
		*adapter_name = name;
	return device;
}

bool gpu_decodes(ID3D11Device *device, Codec codec)
{
	// Profils DXVA (d3d11.h), recopiés ici plutôt que d'instancier toutes les GUID de D3D11
	static const GUID kH264 = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
	static const GUID kHevc = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
	static const GUID kAv1 = {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};
	const GUID &wanted = codec == Codec::HEVC ? kHevc : codec == Codec::AV1 ? kAv1 : kH264;

	ComPtr<ID3D11Device> own;
	if (!device) {
		const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
					     levels, 2, D3D11_SDK_VERSION, &own, nullptr, nullptr)))
			return false;
		device = own.Get();
	}
	ComPtr<ID3D11VideoDevice> video;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video))))
		return false;
	for (UINT i = 0, n = video->GetVideoDecoderProfileCount(); i < n; i++) {
		GUID profile;
		BOOL nv12 = FALSE;
		if (SUCCEEDED(video->GetVideoDecoderProfile(i, &profile)) && profile == wanted &&
		    SUCCEEDED(video->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &nv12)) && nv12)
			return true;
	}
	return false;
}

} // namespace flash
