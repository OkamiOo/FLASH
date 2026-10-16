// Banc d'essai : une fenêtre qui se comporte comme un jeu (image et son), pour mesurer la
// vraie chaîne de capture (Windows.Graphics.Capture + son par application).
// Fenêtre sans bordure 1920×1080 en haut à gauche de l'écran choisi, une image par balayage
// avec le code-barres de la mire (heure de rendu) et un flash à chaque seconde, bip de
// 1 kHz joué à chaque seconde pleine de l'horloge sur la sortie audio choisie, le flash
// s'allumant à la première image rendue à partir de cette seconde.

#include "banc.h"

#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/mire.h>

#include <windows.h>
#include <audioclient.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace flash;

namespace {

constexpr uint32_t W = mire::kWidth, H = mire::kHeight;
std::atomic<bool> g_quit{false};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_CLOSE || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
		g_quit = true;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void fill(std::vector<uint32_t> &img, int x, int y, int w, int h, uint32_t color)
{
	for (int line = y; line < y + h; line++)
		std::fill(img.begin() + (size_t)line * W + x, img.begin() + (size_t)line * W + x + w, color);
}

// Même code-barres que la mire (heure, heure inversée, numéro + numéro inversé), en BGRA
void draw_code(std::vector<uint32_t> &img, uint64_t t, uint32_t frame)
{
	const uint64_t rows[3] = {t, ~t, ((uint64_t)frame << 32) | (uint32_t)~frame};
	for (int r = 0; r < 3; r++)
		for (int b = 0; b < 64; b++)
			fill(img, mire::kX0 + b * mire::kBlock, mire::kY0 + r * mire::kBlock, mire::kBlock, mire::kBlock,
			     (rows[r] >> (63 - b)) & 1 ? 0xFFFFFFFF : 0xFF000000);
}

ComPtr<IMMDevice> find_render_device(const std::wstring &match)
{
	ComPtr<IMMDeviceEnumerator> en;
	ComPtr<IMMDeviceCollection> list;
	UINT n = 0;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
		return nullptr;
	if (match.empty()) {
		ComPtr<IMMDevice> dev;
		en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
		return dev;
	}
	if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list)))
		list->GetCount(&n);
	for (UINT i = 0; i < n; i++) {
		ComPtr<IMMDevice> d;
		ComPtr<IPropertyStore> props;
		PROPVARIANT name;
		PropVariantInit(&name);
		bool ok = SUCCEEDED(list->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) &&
			  SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
			  std::wstring(name.pwszVal).find(match) != std::wstring::npos;
		if (ok)
			FLOG_I("Sortie audio du banc : %s", to_utf8(name.pwszVal).c_str());
		PropVariantClear(&name);
		if (ok)
			return d;
	}
	return nullptr;
}

// Joue les bips : chaque échantillon a une heure de restitution connue grâce à l'horloge
// du périphérique, et le bip démarre pile à chaque seconde après t0.
void play_beeps(const std::wstring &output, uint64_t t0)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	ComPtr<IMMDevice> dev = find_render_device(output);
	ComPtr<IAudioClient> client;
	if (!dev || FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)client.GetAddressOf()))) {
		FLOG_E("Sortie audio du banc introuvable");
		return;
	}

	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = 48000;
	fmt.wBitsPerSample = 32;
	fmt.nBlockAlign = 8;
	fmt.nAvgBytesPerSec = 48000 * 8;
	if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
					      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				      200000, 0, &fmt, nullptr))) {
		FLOG_E("Initialisation de la sortie audio du banc impossible");
		return;
	}

	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	client->SetEventHandle(event);
	ComPtr<IAudioRenderClient> render;
	ComPtr<IAudioClock> clock;
	client->GetService(IID_PPV_ARGS(&render));
	client->GetService(IID_PPV_ARGS(&clock));
	UINT64 freq = 0;
	clock->GetFrequency(&freq);
	UINT32 buffer = 0;
	client->GetBufferSize(&buffer);

	const double w = 2.0 * 3.14159265358979323846 * mire::kBeepHz / 48000.0;
	uint64_t written = 0;
	int64_t origin = 0; // heure de restitution de l'échantillon 0
	bool have_origin = false;
	client->Start();

	while (!g_quit) {
		WaitForSingleObject(event, 100);
		UINT64 pos = 0, qpc = 0;
		if (SUCCEEDED(clock->GetPosition(&pos, &qpc)) && pos > 0) {
			const int64_t o = (int64_t)(qpc * 100) - (int64_t)(pos * 1'000'000'000ULL / freq);
			origin = have_origin ? origin + (o - origin) / 16 : o;
			have_origin = true;
		}
		UINT32 padding = 0;
		client->GetCurrentPadding(&padding);
		const UINT32 frames = buffer - padding;
		BYTE *data = nullptr;
		if (!frames || FAILED(render->GetBuffer(frames, &data)))
			continue;
		float *out = (float *)data;
		for (UINT32 i = 0; i < frames; i++) {
			float v = 0.0f;
			if (have_origin) {
				const int64_t t = origin + (int64_t)((written + i) * 1'000'000'000ULL / 48000);
				if (t >= (int64_t)t0) {
					const uint64_t since = (uint64_t)(t - (int64_t)t0) % 1'000'000'000ULL;
					const uint64_t k = since * 48000 / 1'000'000'000ULL;
					if (k < mire::kBeepSamples)
						v = (float)(0.5 * std::sin(w * (double)k));
				}
			}
			out[2 * i] = out[2 * i + 1] = v;
		}
		render->ReleaseBuffer(frames, 0);
		written += frames;
	}
	client->Stop();
	CloseHandle(event);
	CoUninitialize();
}

} // namespace

int run_banc(const std::string &output, int screen, int duration_s)
{
	enable_precise_timers();
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	WNDCLASSW wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"BancFlash";
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	// En haut à gauche de l'écran choisi (0 = le dernier), sans prendre le focus
	int x = 0, y = 0;
	const std::vector<ScreenInfo> screens = list_screens();
	if (screen <= 0)
		screen = (int)screens.size();
	for (const ScreenInfo &s : screens) {
		if (s.index == screen) {
			x = s.x;
			y = s.y;
		}
	}
	HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Banc FLASH", WS_POPUP, x, y, W, H, nullptr,
				    nullptr, wc.hInstance, nullptr);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
			  D3D11_SDK_VERSION, &device, nullptr, &ctx);
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIFactory2> factory;
	device.As(&dxgi);
	dxgi->GetAdapter(&adapter);
	adapter->GetParent(IID_PPV_ARGS(&factory));

	DXGI_SWAP_CHAIN_DESC1 sd{};
	sd.Width = W;
	sd.Height = H;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> swap;
	if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swap))) {
		FLOG_E("Création de la chaîne d'affichage impossible");
		return 1;
	}

	// Fond dessiné une fois dans une texture du GPU ; à chaque image, seules les zones qui
	// changent y sont envoyées, puis la texture est copiée dans la chaîne d'affichage.
	std::vector<uint32_t> img((size_t)W * H);
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			const uint32_t g = 40 + x * 60 / W;
			img[(size_t)y * W + x] = 0xFF000000 | (g << 16) | (g << 8) | g;
		}
	const std::vector<uint32_t> background = img;
	D3D11_TEXTURE2D_DESC td{};
	td.Width = W;
	td.Height = H;
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA init{img.data(), W * 4, 0};
	ComPtr<ID3D11Texture2D> canvas;
	device->CreateTexture2D(&td, &init, &canvas);
	auto upload = [&](int x, int y, int w, int h) {
		const D3D11_BOX box{(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
		ctx->UpdateSubresource(canvas.Get(), 0, &box, img.data() + (size_t)y * W + x, W * 4, 0);
	};
	const int code_w = 64 * mire::kBlock, code_h = 3 * mire::kBlock;
	const int bar_y = 780, bar_h = H - 820;
	bool flash_on = false;

	// Flashs et bips sur des secondes pleines de l'horloge commune : l'analyse les retrouve
	const uint64_t t0 = (now_ns() / 1'000'000'000ULL + 1) * 1'000'000'000ULL;
	const uint64_t stop_at = duration_s > 0 ? t0 + (uint64_t)duration_s * 1'000'000'000ULL : 0;
	const std::wstring out_w = to_wide(output);
	std::thread audio([&] { play_beeps(out_w, t0); });
	FLOG_I("Banc d'essai en cours (Échap pour quitter)");

	// Une image par balayage de l'écran, rendue juste après le retour de trame : Windows
	// n'en saute aucune. Le flash reste allumé 1/60 s, quelle que soit la fréquence de l'écran.
	ComPtr<IDXGIOutput> screen_out;
	swap->GetContainingOutput(&screen_out);
	for (uint32_t n = 0; !g_quit; n++) {
		if (screen_out)
			screen_out->WaitForVBlank();
		else
			sleep_until_ns(now_ns() + 16'666'667ULL);
		const uint64_t t = now_ns();
		if (stop_at && t >= stop_at)
			break;

		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		// Barre mobile : on efface toute sa bande puis on la redessine
		std::copy(background.begin() + (size_t)bar_y * W, background.begin() + (size_t)(bar_y + bar_h) * W,
			  img.begin() + (size_t)bar_y * W);
		fill(img, (int)((n * 12) % (W - 64)), bar_y, 64, bar_h, 0xFFC8C8C8);
		upload(0, bar_y, W, bar_h);

		draw_code(img, t, n);
		upload(mire::kX0, mire::kY0, code_w, code_h);

		const bool flash = t >= t0 && (t - t0) % 1'000'000'000ULL < 16'666'667ULL;
		if (flash != flash_on) {
			if (flash) {
				fill(img, mire::kFlashX, mire::kFlashY, mire::kFlashSize, mire::kFlashSize, 0xFFFFFFFF);
			} else {
				for (int line = mire::kFlashY; line < mire::kFlashY + mire::kFlashSize; line++)
					std::copy(background.begin() + (size_t)line * W + mire::kFlashX,
						  background.begin() + (size_t)line * W + mire::kFlashX + mire::kFlashSize,
						  img.begin() + (size_t)line * W + mire::kFlashX);
			}
			upload(mire::kFlashX, mire::kFlashY, mire::kFlashSize, mire::kFlashSize);
			flash_on = flash;
		}

		ComPtr<ID3D11Texture2D> back;
		swap->GetBuffer(0, IID_PPV_ARGS(&back));
		ctx->CopyResource(back.Get(), canvas.Get());
		swap->Present(0, 0);
	}

	g_quit = true;
	audio.join();
	DestroyWindow(hwnd);
	return 0;
}
