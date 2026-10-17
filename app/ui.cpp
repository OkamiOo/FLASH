#include "ui.h"
#include "engine.h"
#include "resource.h"

#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/preview.h>

#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <thread>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
using namespace flash;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_TO_PAGE = WM_APP + 2;
constexpr UINT kMenuOpen = 1, kMenuQuit = 2;
constexpr UINT_PTR kTimerStatus = 1, kTimerSettings = 2;
constexpr wchar_t kClass[] = L"FlashFenetre";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kMinWidth = 720, kMinHeight = 560;     // en pixels à 100 %
constexpr int kStartWidth = 1100, kStartHeight = 760;
constexpr uint32_t kThumbW = 320, kThumbH = 180;

// Les opérations lentes (redémarrer l'envoi, lister les fenêtres) passent par un seul fil de
// travail, pour que la fenêtre ne se fige jamais.
class Worker {
public:
	Worker() : thread_([this] { run(); }) {}
	~Worker()
	{
		{
			std::lock_guard lock(mutex_);
			stop_ = true;
		}
		cv_.notify_one();
		thread_.join();
	}
	void post(std::function<void()> task)
	{
		{
			std::lock_guard lock(mutex_);
			tasks_.push_back(std::move(task));
		}
		cv_.notify_one();
	}
	size_t pending()
	{
		std::lock_guard lock(mutex_);
		return tasks_.size();
	}

private:
	void run()
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		for (;;) {
			std::function<void()> task;
			{
				std::unique_lock lock(mutex_);
				cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
				if (stop_)
					break;
				task = std::move(tasks_.front());
				tasks_.pop_front();
			}
			task();
		}
		CoUninitialize();
	}

	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> tasks_;
	bool stop_ = false;
	std::thread thread_;
};

enum class Page { Send, Watch, Devices, Settings };

struct App {
	HWND hwnd = nullptr;
	Engine engine;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> web;
	NOTIFYICONDATAW tray{};
	bool told_background = false;
	std::unique_ptr<Worker> worker;
	std::atomic<Page> page{Page::Send};
	std::string last_lists;
	EngineSettings pending; // réglages en attente (temporisés)
	bool has_pending = false;

	// Vumètres et aperçus, seulement quand la fenêtre est visible sur la page Envoyer
	std::thread live;
	std::atomic<bool> live_stop{false};
};
App *g = nullptr;

std::wstring exe_path()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	return path;
}

bool run_at_startup()
{
	wchar_t value[MAX_PATH * 2];
	DWORD size = sizeof(value);
	return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"FLASH", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

void set_run_at_startup(bool on)
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
		return;
	if (on) {
		const std::wstring cmd = L"\"" + exe_path() + L"\" --demarrage";
		RegSetValueExW(key, L"FLASH", 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
	} else {
		RegDeleteValueW(key, L"FLASH");
	}
	RegCloseKey(key);
}

std::wstring page_html()
{
	HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_UI), MAKEINTRESOURCEW(10)); // RT_RCDATA
	HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
	if (!data)
		return L"<p>Interface introuvable</p>";
	return to_wide(std::string((const char *)LockResource(data), SizeofResource(nullptr, res)));
}

// Envoi vers la page, depuis n'importe quel fil : la page n'accepte que le fil de la fenêtre
void to_page(const json &message)
{
	auto *text = new std::string(message.dump());
	if (!PostMessageW(g->hwnd, WM_TO_PAGE, 0, (LPARAM)text))
		delete text; // fenêtre déjà fermée
}

bool window_shown()
{
	return IsWindowVisible(g->hwnd) && !IsIconic(g->hwnd);
}

// ---------- Icônes des applis ----------

std::mutex g_icons_mutex;
std::map<std::string, std::string> g_icons; // chemin de l'exécutable -> image PNG en data URL

std::string app_icon(const std::string &path)
{
	if (path.empty())
		return "";
	{
		std::lock_guard lock(g_icons_mutex);
		if (auto it = g_icons.find(path); it != g_icons.end())
			return it->second;
	}
	std::string url;
	SHFILEINFOW sfi{};
	if (SHGetFileInfoW(to_wide(path).c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
		constexpr int size = 32;
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = size;
		bi.bmiHeader.biHeight = -size;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void *bits = nullptr;
		HDC dc = CreateCompatibleDC(nullptr);
		HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (bmp && bits) {
			HGDIOBJ old = SelectObject(dc, bmp);
			std::memset(bits, 0, size * size * 4);
			DrawIconEx(dc, 0, 0, sfi.hIcon, size, size, 0, nullptr, DI_NORMAL);
			GdiFlush();
			SelectObject(dc, old);
			// Vieille icône sans couche alpha : tout ce qui est dessiné devient opaque
			uint8_t *p = (uint8_t *)bits;
			bool alpha = false;
			for (int i = 0; i < size * size && !alpha; i++)
				alpha = p[i * 4 + 3] != 0;
			if (!alpha)
				for (int i = 0; i < size * size; i++)
					if (p[i * 4] || p[i * 4 + 1] || p[i * 4 + 2])
						p[i * 4 + 3] = 255;
			std::vector<uint8_t> png;
			if (encode_png(p, size, size, size * 4, png))
				url = "data:image/png;base64," + base64(png);
		}
		if (bmp)
			DeleteObject(bmp);
		DeleteDC(dc);
		DestroyIcon(sfi.hIcon);
	}
	std::lock_guard lock(g_icons_mutex);
	g_icons[path] = url;
	return url;
}

// ---------- Ce que la page affiche ----------

json sources_json()
{
	json list = json::array();
	for (const SourceChoice &s : g->engine.sources())
		list.push_back({{"type", s.kind}, {"valeur", s.value}});
	return list;
}

json sources_message()
{
	return {{"type", "sources"}, {"sources", sources_json()}, {"fenetres", g->engine.windows()}};
}

json settings_json()
{
	const EngineSettings s = g->engine.settings();
	return {{"debit", s.video_kbps},
		{"codec", format_key(s.codec)},
		{"adresse", s.address},
		{"latence", s.latency_ms},
		{"demarrage", run_at_startup()},
		{"curseur", s.cursor},
		{"taille", std::to_string(s.max_w) + "x" + std::to_string(s.max_h)},
		{"encodeur", s.encoder},
		{"version", FLASH_VERSION}};
}

json lists()
{
	json j = {{"type", "listes"}};
	j["ecrans"] = json::array();
	for (const ScreenInfo &s : list_screens())
		j["ecrans"].push_back(
			{{"numero", s.index}, {"largeur", s.width}, {"hauteur", s.height}, {"hz", s.hz}, {"principal", s.primary}});
	// Fenêtres ouvertes, une par exécutable (c'est lui qui désigne la fenêtre envoyée)
	j["ouvertes"] = json::array();
	std::vector<std::string> seen;
	for (const WindowInfo &w : list_windows()) {
		if (_stricmp(w.exe.c_str(), "flash.exe") == 0 ||
		    std::find_if(seen.begin(), seen.end(), [&](const std::string &e) { return _stricmp(e.c_str(), w.exe.c_str()) == 0; }) !=
			    seen.end())
			continue;
		seen.push_back(w.exe);
		j["ouvertes"].push_back({{"titre", w.title}, {"exe", w.exe}, {"icone", app_icon(w.path)}});
	}
	j["fenetres"] = g->engine.windows();
	j["applis"] = json::array();
	for (const AudioAppInfo &a : list_audio_apps())
		j["applis"].push_back({{"exe", a.exe}, {"joue", a.playing}, {"icone", app_icon(a.path)}});
	j["micros"] = list_microphones();
	j["sources"] = sources_json();
	j["volumes"] = g->engine.volumes();
	j["reglages"] = settings_json();
	if (ObsClient *obs = g->engine.obs())
		j["obs"] = obs->catalog();
	return j;
}

// Listes envoyées seulement si elles ont changé : la page ne se redessine pas pour rien
void send_lists(bool force)
{
	json j = lists();
	const std::string text = j.dump();
	if (!force && text == g->last_lists)
		return;
	g->last_lists = text;
	to_page(j);
}

void send_status()
{
	json st = g->engine.status();
	st["type"] = "etat";
	to_page(st);
}

void send_senders(bool refresh)
{
	to_page(g->engine.senders_json(refresh));
}

// ---------- Vumètres et aperçus ----------

void live_loop()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	AudioMeters meters;
	std::unique_ptr<Thumbnailer> thumbs;
	uint64_t next_frames = 0, next_snapshot = 0;
	size_t turn = 0;
	std::vector<uint8_t> jpg;

	auto send_thumb = [&](const std::string &key, ID3D11Texture2D *tex) {
		if (tex && thumbs->jpeg(tex, kThumbW, kThumbH, jpg))
			to_page({{"type", "apercu"}, {"cle", key}, {"img", "data:image/jpeg;base64," + base64(jpg)}});
	};

	while (!g->live_stop) {
		Sleep(66);
		const bool visible = window_shown() && g->page == Page::Send;
		if (ObsClient *obs = g->engine.obs())
			obs->set_previews(visible); // aperçus des scènes et sources, rendus par OBS
		if (!visible)
			continue;

		// Niveaux : la piste envoyée si la source l'est, sinon le niveau de Windows au volume choisi
		const auto windows_levels = meters.read();
		const auto sent = g->engine.levels();
		const auto volumes = g->engine.volumes();
		json n = json::object();
		for (const auto &[key, peak] : windows_levels) {
			auto v = volumes.find(key);
			n[key] = peak * (v != volumes.end() ? v->second : 1.0f);
		}
		for (const auto &[key, peak] : sent)
			n[key] = peak;
		to_page({{"type", "niveaux"}, {"n", n}});

		if (!thumbs) {
			if (!g->engine.device())
				continue;
			thumbs = std::make_unique<Thumbnailer>(g->engine.device());
		}
		const uint64_t now = now_ns();
		const auto sources = g->engine.sources();
		auto sending = [&](const std::string &key) {
			return std::any_of(sources.begin(), sources.end(), [&](const SourceChoice &s) { return s.key() == key; });
		};

		// Sources envoyées : leur dernière image, 4 fois par seconde
		if (now >= next_frames) {
			next_frames = now + 250'000'000ULL;
			for (const SourceChoice &s : sources)
				if (s.kind == "ecran" || s.kind == "fenetre" || s.kind.starts_with("obs-"))
					send_thumb(s.key(), g->engine.last_frame(s.key()).Get());
		}

		// Les autres écrans et fenêtres : une capture ponctuelle, à tour de rôle
		if (now >= next_snapshot) {
			next_snapshot = now + 400'000'000ULL;
			std::vector<std::pair<int, std::string>> idle;
			for (const ScreenInfo &sc : list_screens())
				if (!sending("ecran|" + std::to_string(sc.index)))
					idle.push_back({sc.index, ""});
			for (const std::string &w : g->engine.windows())
				if (!sending("fenetre|" + w))
					idle.push_back({0, w});
			if (!idle.empty()) {
				const auto &[screen, window] = idle[turn++ % idle.size()];
				ComPtr<ID3D11Texture2D> tex = snapshot(g->engine.device().Get(), screen, window);
				send_thumb(screen > 0 ? "ecran|" + std::to_string(screen) : "fenetre|" + window, tex.Get());
			}
		}
	}
	CoUninitialize();
}

// ---------- Messages de la page ----------

SourceChoice source_of(const json &m)
{
	return {m.value("type", ""), m.value("valeur", "")};
}

void copy_to_clipboard(const std::string &text)
{
	const std::wstring w = to_wide(text);
	if (!OpenClipboard(g->hwnd))
		return;
	EmptyClipboard();
	if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
		std::memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
		GlobalUnlock(h);
		if (!SetClipboardData(CF_UNICODETEXT, h))
			GlobalFree(h);
	}
	CloseClipboard();
}

void on_page_message(const json &m)
{
	const std::string action = m.value("action", "");
	if (action == "pret") {
		g->worker->post([] {
			send_lists(true);
			send_status();
		});
	} else if (action == "page") {
		const std::string name = m.value("nom", "");
		g->page = name == "regarder" ? Page::Watch : name == "appareils" ? Page::Devices
			: name == "reglages"  ? Page::Settings
					      : Page::Send;
		if (g->page == Page::Devices)
			g->worker->post([] { send_senders(false); });
	} else if (action == "source") {
		const SourceChoice s = source_of(m);
		const bool on = m.value("actif", false);
		g->worker->post([s, on] {
			g->engine.set_source(s, on);
			to_page(sources_message());
		});
	} else if (action == "volume") {
		const SourceChoice s = source_of(m);
		const float volume = std::clamp(m.value("volume", 1.0f), 0.0f, 1.0f);
		g->worker->post([s, volume] { g->engine.set_volume(s, volume); });
	} else if (action == "ajouter_fenetre" || action == "retirer_fenetre") {
		const std::string exe = m.value("exe", "");
		const bool add = action == "ajouter_fenetre";
		g->worker->post([exe, add] {
			if (add)
				g->engine.add_window(exe);
			else
				g->engine.remove_window(exe);
			to_page(sources_message());
		});
	} else if (action == "copier") {
		copy_to_clipboard(m.value("texte", ""));
	} else if (action == "reglages") {
		// Appliqués 400 ms après le dernier changement : un curseur qu'on bouge ne refait pas
		// les encodeurs à chaque cran
		EngineSettings s = g->has_pending ? g->pending : g->engine.settings();
		s.video_kbps = (uint32_t)std::clamp(m.value("debit", (int)s.video_kbps), 6000, 80000);
		s.codec = format_from_key(m.value("codec", std::string(format_key(s.codec))));
		s.address = m.value("adresse", s.address);
		s.latency_ms = m.value("latence", s.latency_ms);
		s.cursor = m.value("curseur", s.cursor);
		s.encoder = m.value("encodeur", s.encoder);
		std::sscanf(m.value("taille", std::string()).c_str(), "%ux%u", &s.max_w, &s.max_h);
		if (m.value("demarrage", false) != run_at_startup())
			set_run_at_startup(m.value("demarrage", false));
		g->pending = s;
		g->has_pending = true;
		SetTimer(g->hwnd, kTimerSettings, 400, nullptr);
	} else if (action == "nouveau_code") {
		g->worker->post([] {
			g->engine.new_code();
			send_status();
		});
	} else if (action == "retirer") {
		const std::string id = m.value("id", "");
		g->worker->post([id] {
			g->engine.forget_receiver(id);
			send_status();
		});
	} else if (action == "chercher") {
		g->worker->post([] { send_senders(true); });
	} else if (action == "appairer") {
		const std::string address = m.value("adresse", "");
		const uint16_t port = (uint16_t)m.value("port", 0);
		const std::string code = m.value("code", "");
		const std::string id = m.value("id", "");
		g->worker->post([address, port, code, id] {
			const PairingResult r = g->engine.pair(address, port, code);
			to_page({{"type", "appairage"}, {"id", id}, {"ok", r.ok}, {"erreur", r.error}, {"nom", r.sender.name}});
			send_senders(false);
		});
	} else if (action == "oublier") {
		const std::string id = m.value("id", "");
		g->worker->post([id] {
			g->engine.forget_sender(id);
			send_senders(false);
		});
	} else if (action == "regarder") {
		WatchRequest r;
		r.sender = m.value("moi", false) ? "moi" : m.value("id", "");
		r.video = m.value("video", "");
		if (m.contains("son") && m["son"].is_array()) {
			for (const auto &s : m["son"])
				if (s.is_string())
					r.audio.push_back(s.get<std::string>());
		}
		r.output = m.value("sortie", "");
		const std::string id = m.value("id", "");
		g->worker->post([r, id] {
			std::string error;
			if (!g->engine.watch(r, error))
				to_page({{"type", "appairage"}, {"id", id}, {"ok", false}, {"erreur", error}});
			send_senders(false);
			send_status();
		});
	} else if (action == "fermer_vue") {
		const std::string id = m.value("id", "");
		g->worker->post([id] {
			g->engine.close_view(id);
			send_senders(false);
			send_status();
		});
	}
}

void create_webview()
{
	PWSTR base = nullptr;
	std::wstring data_dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
		data_dir = std::wstring(base) + L"\\FLASH\\Interface";
	CoTaskMemFree(base);

	const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
		nullptr, data_dir.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
				if (FAILED(result) || !env)
					return result;
				return env->CreateCoreWebView2Controller(
					g->hwnd,
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {
							if (FAILED(result) || !controller)
								return result;
							g->controller = controller;
							controller->get_CoreWebView2(&g->web);

							ComPtr<ICoreWebView2Settings> settings;
							g->web->get_Settings(&settings);
							settings->put_AreDevToolsEnabled(FALSE);
							settings->put_AreDefaultContextMenusEnabled(FALSE);
							settings->put_IsZoomControlEnabled(FALSE);
							settings->put_IsStatusBarEnabled(FALSE);

							EventRegistrationToken token;
							g->web->add_WebMessageReceived(
								Callback<ICoreWebView2WebMessageReceivedEventHandler>(
									[](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
										LPWSTR text = nullptr;
										if (SUCCEEDED(args->get_WebMessageAsJson(&text))) {
											const json m = json::parse(to_utf8(text), nullptr, false);
											if (m.is_object())
												on_page_message(m);
											CoTaskMemFree(text);
										}
										return S_OK;
									})
									.Get(),
								&token);

							RECT rc;
							GetClientRect(g->hwnd, &rc);
							controller->put_Bounds(rc);
							g->web->NavigateToString(page_html().c_str());
							return S_OK;
						})
						.Get());
			})
			.Get());
	if (FAILED(hr))
		MessageBoxW(g->hwnd,
			    L"Le composant WebView2 de Windows est introuvable. Installez « Microsoft Edge WebView2 Runtime » puis relancez FLASH.",
			    L"FLASH", MB_ICONERROR);
}

void show_window()
{
	if (IsIconic(g->hwnd))
		ShowWindow(g->hwnd, SW_RESTORE);
	else
		ShowWindow(g->hwnd, SW_SHOW);
	SetForegroundWindow(g->hwnd);
	if (g->worker && !g->worker->pending())
		g->worker->post([] { send_lists(false); });
}

void tray_menu()
{
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, kMenuOpen, L"Ouvrir FLASH");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quitter");
	POINT p;
	GetCursorPos(&p);
	SetForegroundWindow(g->hwnd);
	const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, g->hwnd, nullptr);
	DestroyMenu(menu);
	if (cmd == kMenuOpen)
		show_window();
	else if (cmd == kMenuQuit)
		DestroyWindow(g->hwnd);
}

// Taille de départ : 1100×760 à l'échelle de l'écran, au centre de l'écran de la souris, réduite
// pour tenir dans la zone de travail
RECT start_rect()
{
	POINT p;
	GetCursorPos(&p);
	HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi{sizeof(mi)};
	GetMonitorInfoW(mon, &mi);
	UINT dx = 96, dy = 96;
	GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
	const RECT &work = mi.rcWork;
	const int ww = work.right - work.left, wh = work.bottom - work.top;
	RECT r{0, 0, MulDiv(kStartWidth, dx, 96), MulDiv(kStartHeight, dx, 96)};
	AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dx);
	const int w = std::min<int>(r.right - r.left, ww * 95 / 100);
	const int h = std::min<int>(r.bottom - r.top, wh * 95 / 100);
	const int x = work.left + (ww - w) / 2, y = work.top + (wh - h) / 2;
	return {x, y, x + w, y + h};
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	// L'installeur demande à FLASH de quitter avant de remplacer ses fichiers
	static const UINT quit_request = RegisterWindowMessageW(L"FLASH-quitter");
	if (msg == quit_request) {
		DestroyWindow(hwnd);
		return 0;
	}
	switch (msg) {
	case WM_SIZE:
		if (g && g->controller) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			g->controller->put_Bounds(rc);
		}
		return 0;
	case WM_GETMINMAXINFO: {
		// Plus petite fenêtre utile : 720×560 à l'échelle de l'écran
		const UINT dpi = GetDpiForWindow(hwnd);
		RECT r{0, 0, MulDiv(kMinWidth, dpi, 96), MulDiv(kMinHeight, dpi, 96)};
		AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
		((MINMAXINFO *)lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
		return 0;
	}
	case WM_DPICHANGED: {
		const RECT *r = (const RECT *)lp;
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_CLOSE:
		// Fermer la fenêtre ne coupe pas l'envoi : FLASH reste dans la zone de notification
		ShowWindow(hwnd, SW_HIDE);
		if (!g->told_background) {
			g->told_background = true;
			g->tray.uFlags = NIF_INFO;
			wcscpy_s(g->tray.szInfoTitle, L"FLASH continue en arrière-plan");
			wcscpy_s(g->tray.szInfo, L"Clic droit sur l'icône pour quitter.");
			Shell_NotifyIconW(NIM_MODIFY, &g->tray);
		}
		return 0;
	case WM_TRAY:
		if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP)
			show_window();
		else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
			tray_menu();
		return 0;
	case WM_TO_PAGE: {
		std::unique_ptr<std::string> text((std::string *)lp);
		if (g->web)
			g->web->PostWebMessageAsJson(to_wide(*text).c_str());
		return 0;
	}
	case WM_TIMER:
		if (wp == kTimerSettings) {
			KillTimer(hwnd, kTimerSettings);
			const EngineSettings s = g->pending;
			g->has_pending = false;
			g->worker->post([s] { g->engine.set_settings(s); });
			return 0;
		}
		if (wp == kTimerStatus) {
			// État chaque seconde ; listes (applis, fenêtres) toutes les 5 s si elles ont changé ;
			// émetteurs du réseau toutes les 5 s sur la page Regarder. Fenêtre cachée : on retire
			// seulement les visionneuses fermées.
			static unsigned ticks = 0;
			ticks++;
			if (g->worker->pending() == 0) {
				if (!window_shown()) {
					g->worker->post([] { g->engine.views(); });
				} else {
					const bool every5 = ticks % 5 == 0;
					const Page page = g->page;
					g->worker->post([every5, page] {
						send_status();
						if (every5 && page == Page::Send)
							send_lists(false);
						if (every5 && page == Page::Watch)
							send_senders(true);
					});
				}
			}
			return 0;
		}
		return 0;
	case WM_DESTROY:
		Shell_NotifyIconW(NIM_DELETE, &g->tray);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int run_ui(bool hidden)
{
	// Une seule appli à la fois : un second lancement montre la fenêtre de la première
	HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\FLASH-appli");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (HWND other = FindWindowW(kClass, nullptr)) {
			ShowWindow(other, SW_SHOWNORMAL);
			SetForegroundWindow(other);
		}
		return 0;
	}

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	enable_precise_timers();

	App app;
	g = &app;
	if (!app.engine.start(true)) {
		MessageBoxW(nullptr, L"Aucune carte graphique utilisable pour l'encodage.", L"FLASH", MB_ICONERROR);
		return 1;
	}
	app.worker = std::make_unique<Worker>();

	HINSTANCE inst = GetModuleHandleW(nullptr);
	HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_FLASH));
	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.hIcon = icon;
	wc.hIconSm = icon;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kClass;
	RegisterClassExW(&wc);

	const RECT r = start_rect();
	app.hwnd = CreateWindowExW(0, kClass, L"FLASH", WS_OVERLAPPEDWINDOW, r.left, r.top, r.right - r.left, r.bottom - r.top,
				   nullptr, nullptr, inst, nullptr);

	app.tray.cbSize = sizeof(app.tray);
	app.tray.hWnd = app.hwnd;
	app.tray.uID = 1;
	app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	app.tray.uCallbackMessage = WM_TRAY;
	app.tray.hIcon = icon;
	wcscpy_s(app.tray.szTip, L"FLASH");
	Shell_NotifyIconW(NIM_ADD, &app.tray);

	create_webview();
	SetTimer(app.hwnd, kTimerStatus, 1000, nullptr);
	if (ObsClient *obs = app.engine.obs()) {
		obs->set_listener(
			[](const json &catalog) {
				json m = catalog;
				m["type"] = "obs";
				to_page(m);
			},
			[](const std::string &key, const std::vector<uint8_t> &jpeg) {
				to_page({{"type", "apercu"}, {"cle", key}, {"img", "data:image/jpeg;base64," + base64(jpeg)}});
			});
	}
	if (!hidden)
		ShowWindow(app.hwnd, SW_SHOWNORMAL);
	app.live = std::thread(live_loop);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	app.live_stop = true;
	app.live.join();
	app.worker.reset();
	app.engine.stop();
	app.controller.Reset();
	app.web.Reset();
	g = nullptr;
	CoUninitialize();
	CloseHandle(single);
	return 0;
}
