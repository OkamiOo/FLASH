#include "ui.h"
#include "engine.h"
#include "resource.h"

#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/log.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <condition_variable>
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
constexpr wchar_t kClass[] = L"FlashFenetre";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

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

struct App {
	HWND hwnd = nullptr;
	Engine engine;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> web;
	NOTIFYICONDATAW tray{};
	bool told_background = false;
	std::unique_ptr<Worker> worker;
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
	PostMessageW(g->hwnd, WM_TO_PAGE, 0, (LPARAM) new std::string(message.dump()));
}

json lists(bool keep_draft)
{
	json j = {{"type", "listes"}, {"garder", keep_draft}};
	j["ecrans"] = json::array();
	for (const ScreenInfo &s : list_screens())
		j["ecrans"].push_back(
			{{"numero", s.index}, {"largeur", s.width}, {"hauteur", s.height}, {"hz", s.hz}, {"principal", s.primary}});
	j["fenetres"] = json::array();
	for (const WindowInfo &w : list_windows()) {
		if (w.exe != "flash.exe")
			j["fenetres"].push_back({{"titre", w.title}, {"exe", w.exe}});
	}
	j["applis"] = json::array();
	for (const AudioAppInfo &a : list_audio_apps())
		j["applis"].push_back({{"exe", a.exe}, {"joue", a.playing}});
	j["micros"] = list_microphones();
	j["sources"] = json::array();
	for (const SourceChoice &s : g->engine.sources())
		j["sources"].push_back({{"type", s.kind}, {"valeur", s.value}});
	const EngineSettings s = g->engine.settings();
	j["reglages"] = {{"debit", s.video_kbps},
			 {"hevc", s.hevc},
			 {"adresse", s.address},
			 {"latence", s.latency_ms},
			 {"demarrage", run_at_startup()}};
	return j;
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

void on_page_message(const json &m)
{
	const std::string action = m.value("action", "");
	if (action == "pret") {
		g->worker->post([] {
			to_page(lists(false));
			send_status();
			send_senders(true);
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
	} else if (action == "appliquer" && m.contains("sources")) {
		std::vector<SourceChoice> sources;
		for (const auto &s : m["sources"])
			sources.push_back({s.value("type", ""), s.value("valeur", "")});
		g->worker->post([sources] {
			g->engine.set_sources(sources);
			to_page(lists(false));
		});
	} else if (action == "reglages") {
		EngineSettings s = g->engine.settings();
		s.video_kbps = m.value("debit", s.video_kbps);
		s.hevc = m.value("hevc", s.hevc);
		s.address = m.value("adresse", s.address);
		s.latency_ms = m.value("latence", s.latency_ms);
		if (m.value("demarrage", false) != run_at_startup())
			set_run_at_startup(m.value("demarrage", false));
		g->worker->post([s] { g->engine.set_settings(s); });
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
	ShowWindow(g->hwnd, SW_SHOWNORMAL);
	SetForegroundWindow(g->hwnd);
	if (g->worker && !g->worker->pending())
		g->worker->post([] { to_page(lists(true)); });
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

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_SIZE:
		if (g && g->controller) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			g->controller->put_Bounds(rc);
		}
		return 0;
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
	case WM_TIMER: {
		// État chaque seconde, émetteurs du réseau toutes les 5 s, tant que la fenêtre est ouverte ;
		// fenêtre cachée, on retire quand même les visionneuses fermées
		static unsigned ticks = 0;
		ticks++;
		if (g->worker->pending() == 0) {
			if (!IsWindowVisible(hwnd))
				g->worker->post([] { g->engine.views(); });
			else if (ticks % 5 == 0)
				g->worker->post([] {
					send_status();
					send_senders(true);
				});
			else
				g->worker->post(send_status);
		}
		return 0;
	}
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

	const UINT dpi = GetDpiForSystem();
	app.hwnd = CreateWindowExW(0, kClass, L"FLASH", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
				   MulDiv(1000, dpi, 96), MulDiv(700, dpi, 96), nullptr, nullptr, inst, nullptr);

	app.tray.cbSize = sizeof(app.tray);
	app.tray.hWnd = app.hwnd;
	app.tray.uID = 1;
	app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	app.tray.uCallbackMessage = WM_TRAY;
	app.tray.hIcon = icon;
	wcscpy_s(app.tray.szTip, L"FLASH");
	Shell_NotifyIconW(NIM_ADD, &app.tray);

	create_webview();
	SetTimer(app.hwnd, 1, 1000, nullptr);
	if (!hidden)
		ShowWindow(app.hwnd, SW_SHOWNORMAL);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	app.worker.reset();
	app.engine.stop();
	app.controller.Reset();
	app.web.Reset();
	g = nullptr;
	CoUninitialize();
	CloseHandle(single);
	return 0;
}
