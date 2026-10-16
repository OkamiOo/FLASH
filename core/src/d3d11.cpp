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

} // namespace flash
