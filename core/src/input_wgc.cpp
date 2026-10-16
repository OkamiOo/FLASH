#include "converter.h"
#include "flash/clock.h"
#include "flash/inputs.h"
#include "flash/log.h"

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

namespace flash {

// ---------- Utilitaires ----------

std::wstring to_wide(const std::string &s)
{
	if (s.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string to_utf8(const std::wstring &w)
{
	if (w.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

void fit_size(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h, uint32_t &w, uint32_t &h)
{
	w = src_w;
	h = src_h;
	if (w > max_w || h > max_h) {
		if ((uint64_t)w * max_h > (uint64_t)h * max_w) {
			h = (uint32_t)((uint64_t)h * max_w / w);
			w = max_w;
		} else {
			w = (uint32_t)((uint64_t)w * max_h / h);
			h = max_h;
		}
	}
	w = std::max<uint32_t>(2, w & ~1u);
	h = std::max<uint32_t>(2, h & ~1u);
}

namespace {

struct Screen {
	HMONITOR monitor;
	RECT rect;
	bool primary;
	std::wstring device;
};

std::vector<Screen> screens()
{
	std::vector<Screen> out;
	EnumDisplayMonitors(
		nullptr, nullptr,
		[](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
			MONITORINFOEXW mi{};
			mi.cbSize = sizeof(mi);
			if (GetMonitorInfoW(m, &mi))
				((std::vector<Screen> *)p)
					->push_back({m, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.szDevice});
			return TRUE;
		},
		(LPARAM)&out);
	// Écran 1 = l'écran principal, puis de gauche à droite
	std::sort(out.begin(), out.end(), [](const Screen &a, const Screen &b) {
		if (a.primary != b.primary)
			return a.primary;
		return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
	});
	return out;
}

std::wstring exe_of(HWND hwnd)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	std::wstring exe;
	if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t path[MAX_PATH];
		DWORD len = MAX_PATH;
		if (QueryFullProcessImageNameW(h, 0, path, &len)) {
			exe = path;
			exe = exe.substr(exe.find_last_of(L"\\/") + 1);
		}
		CloseHandle(h);
	}
	return exe;
}

bool capturable_window(HWND hwnd)
{
	if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
		return false;
	const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW)
		return false;
	BOOL cloaked = FALSE;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	return !cloaked && GetWindowTextLengthW(hwnd) > 0;
}

std::wstring lower(std::wstring s)
{
	CharLowerBuffW(s.data(), (DWORD)s.size());
	return s;
}

// Fenêtre dont l'exécutable porte ce nom (avec ou sans .exe), sinon dont le titre le contient
HWND find_window(const std::wstring &match)
{
	struct Search {
		std::wstring needle;
		HWND by_exe = nullptr;
		HWND by_title = nullptr;
	} s{lower(match)};

	EnumWindows(
		[](HWND hwnd, LPARAM p) -> BOOL {
			auto *s = (Search *)p;
			if (!capturable_window(hwnd))
				return TRUE;
			const std::wstring exe = lower(exe_of(hwnd));
			if (!s->by_exe && (exe == s->needle || exe == s->needle + L".exe"))
				s->by_exe = hwnd;
			wchar_t title[512];
			GetWindowTextW(hwnd, title, 512);
			if (!s->by_title && lower(title).find(s->needle) != std::wstring::npos)
				s->by_title = hwnd;
			return TRUE;
		},
		(LPARAM)&s);
	return s.by_exe ? s.by_exe : s.by_title;
}

template <typename T> ComPtr<T> dxgi_interface(const winrt::Windows::Foundation::IInspectable &obj)
{
	auto access = obj.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	ComPtr<T> out;
	winrt::check_hresult(access->GetInterface(__uuidof(T), (void **)out.GetAddressOf()));
	return out;
}

// ---------- Entrée ----------

class WgcInput final : public VideoInput {
public:
	WgcInput(int screen, std::wstring window, uint32_t max_w, uint32_t max_h, bool cursor)
		: screen_(screen), window_(std::move(window)), cursor_(cursor)
	{
		uint32_t src_w = 1920, src_h = 1080;
		if (screen_ > 0) {
			const auto list = screens();
			if (screen_ <= (int)list.size()) {
				const RECT &r = list[screen_ - 1].rect;
				src_w = r.right - r.left;
				src_h = r.bottom - r.top;
			}
			name_ = "Écran " + std::to_string(screen_);
		} else {
			name_ = to_utf8(window_);
			if (HWND hwnd = find_window(window_)) {
				RECT r;
				if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
					src_w = r.right - r.left;
					src_h = r.bottom - r.top;
				}
			}
		}
		fit_size(src_w, src_h, max_w, max_h, w_, h_);
	}

	~WgcInput() override { stop(); }

	bool start(ID3D11Device *device, OnFrame on_frame) override
	{
		if (!converter_.init(device, w_, h_))
			return false;
		device_ = device;
		on_frame_ = std::move(on_frame);
		running_ = true;
		thread_ = std::thread(&WgcInput::run, this);
		return true;
	}

	void stop() override
	{
		if (running_.exchange(false) && thread_.joinable())
			thread_.join();
	}

	uint32_t width() const override { return w_; }
	uint32_t height() const override { return h_; }
	std::string name() const override { return name_; }

private:
	wgc::GraphicsCaptureItem create_item()
	{
		auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
		wgc::GraphicsCaptureItem item{nullptr};
		if (screen_ > 0) {
			const auto list = screens();
			if (screen_ > (int)list.size())
				return nullptr;
			winrt::check_hresult(interop->CreateForMonitor(list[screen_ - 1].monitor,
								       winrt::guid_of<wgc::GraphicsCaptureItem>(),
								       winrt::put_abi(item)));
		} else {
			HWND hwnd = find_window(window_);
			if (!hwnd)
				return nullptr;
			winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
								      winrt::put_abi(item)));
		}
		return item;
	}

	bool open()
	{
		wgc::GraphicsCaptureItem item = create_item();
		if (!item)
			return false;

		ComPtr<IDXGIDevice> dxgi;
		device_.As(&dxgi);
		winrt::com_ptr<IInspectable> insp;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()));
		d3d_ = insp.as<wdx::Direct3D11::IDirect3DDevice>();

		size_ = item.Size();
		pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
									     size_);
		session_ = pool_.CreateCaptureSession(item);

		using winrt::Windows::Foundation::Metadata::ApiInformation;
		if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
			wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
			session_.IsBorderRequired(false);
		}
		if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
						      L"IsCursorCaptureEnabled"))
			session_.IsCursorCaptureEnabled(cursor_);

		closed_ = false;
		item_ = item;
		item_closed_ = item.Closed(winrt::auto_revoke, [this](auto &&, auto &&) { closed_ = true; });
		frame_arrived_ = pool_.FrameArrived(winrt::auto_revoke, [this](auto &&pool, auto &&) { on_frame(pool); });
		session_.StartCapture();
		FLOG_I("Capture de « %s » : %dx%d, envoyée en %ux%u", name_.c_str(), size_.Width, size_.Height, w_, h_);
		return true;
	}

	void close()
	{
		frame_arrived_.revoke();
		item_closed_.revoke();
		std::lock_guard lock(frame_mutex_);
		if (session_)
			session_.Close();
		if (pool_)
			pool_.Close();
		session_ = nullptr;
		pool_ = nullptr;
		item_ = nullptr;
	}

	void on_frame(const wgc::Direct3D11CaptureFramePool &pool)
	{
		std::lock_guard lock(frame_mutex_);
		wgc::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
		if (!frame)
			return;

		// Heure de composition de l'image par Windows, en unités de 100 ns de l'horloge QPC
		const uint64_t capture_ns = (uint64_t)frame.SystemRelativeTime().count() * 100;
		const auto content = frame.ContentSize();

		try {
			ComPtr<ID3D11Texture2D> tex = dxgi_interface<ID3D11Texture2D>(frame.Surface());
			if (ID3D11Texture2D *nv12 = converter_.convert(tex.Get()))
				on_frame_(nv12, capture_ns);
		} catch (const winrt::hresult_error &) {
		}

		// Fenêtre redimensionnée : on refait la réserve d'images à la nouvelle taille
		if (content.Width != size_.Width || content.Height != size_.Height) {
			size_ = content;
			pool.Recreate(d3d_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
		}
	}

	void run()
	{
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		bool warned = false;
		while (running_) {
			if (!session_ || closed_) {
				if (session_)
					close();
				try {
					if (open()) {
						warned = false;
					} else if (!warned) {
						FLOG_W("« %s » introuvable pour l'instant, recherche en continu", name_.c_str());
						warned = true;
					}
				} catch (const winrt::hresult_error &e) {
					FLOG_E("Capture de « %s » impossible (0x%08x)", name_.c_str(), (unsigned)e.code());
				}
			}
			for (int i = 0; i < 10 && running_; i++)
				Sleep(100);
		}
		close();
		winrt::uninit_apartment();
	}

	const int screen_;
	const std::wstring window_;
	const bool cursor_;
	std::string name_;
	uint32_t w_ = 0, h_ = 0;

	ComPtr<ID3D11Device> device_;
	Nv12Converter converter_;
	OnFrame on_frame_;
	std::atomic<bool> running_{false};
	std::atomic<bool> closed_{false};
	std::thread thread_;

	std::mutex frame_mutex_;
	wdx::Direct3D11::IDirect3DDevice d3d_{nullptr};
	wgc::GraphicsCaptureItem item_{nullptr};
	wgc::Direct3D11CaptureFramePool pool_{nullptr};
	wgc::GraphicsCaptureSession session_{nullptr};
	winrt::Windows::Graphics::SizeInt32 size_{};
	wgc::GraphicsCaptureItem::Closed_revoker item_closed_;
	wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived_;
};

} // namespace

std::unique_ptr<VideoInput> make_screen_input(int index, uint32_t max_w, uint32_t max_h, bool cursor)
{
	return std::make_unique<WgcInput>(index, L"", max_w, max_h, cursor);
}

std::unique_ptr<VideoInput> make_window_input(const std::string &match, uint32_t max_w, uint32_t max_h, bool cursor)
{
	return std::make_unique<WgcInput>(0, to_wide(match), max_w, max_h, cursor);
}

std::vector<ScreenInfo> list_screens()
{
	std::vector<ScreenInfo> out;
	int i = 1;
	for (const Screen &s : screens()) {
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		EnumDisplaySettingsW(s.device.c_str(), ENUM_CURRENT_SETTINGS, &mode);
		std::wstring adapter;
		DISPLAY_DEVICEW dd{};
		dd.cb = sizeof(dd);
		for (DWORD k = 0; EnumDisplayDevicesW(nullptr, k, &dd, 0); k++) {
			if (s.device == dd.DeviceName)
				adapter = dd.DeviceString;
			dd.cb = sizeof(dd);
		}
		out.push_back({i++, to_utf8(s.device), (int)s.rect.left, (int)s.rect.top, (int)(s.rect.right - s.rect.left),
			       (int)(s.rect.bottom - s.rect.top), (int)mode.dmDisplayFrequency, to_utf8(adapter), s.primary});
	}
	return out;
}

std::vector<WindowInfo> list_windows()
{
	std::vector<WindowInfo> out;
	EnumWindows(
		[](HWND hwnd, LPARAM p) -> BOOL {
			if (capturable_window(hwnd)) {
				wchar_t title[512];
				GetWindowTextW(hwnd, title, 512);
				((std::vector<WindowInfo> *)p)->push_back({to_utf8(title), to_utf8(exe_of(hwnd))});
			}
			return TRUE;
		},
		(LPARAM)&out);
	return out;
}

} // namespace flash
