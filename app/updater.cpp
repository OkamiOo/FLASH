#include "updater.h"

#include <flash/log.h>
#include <flash/update.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace flash;

namespace updater {

namespace {

// Clé publique des mises à jour de FLASH. La clé privée ne quitte jamais mon PC.
constexpr uint8_t kPublicKey[32] = {0xf1, 0xce, 0xf8, 0x5d, 0x8f, 0x1e, 0x4e, 0xd0, 0xef, 0xdd, 0x28,
				    0x90, 0xdd, 0xe5, 0x35, 0x9a, 0x5f, 0xdd, 0x47, 0xd1, 0x4f, 0x92,
				    0xbc, 0x93, 0x3d, 0x49, 0xa4, 0xd8, 0x9b, 0x53, 0xa1, 0x82};

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Adresses de publication, dans l'ordre : aucune tant que l'hébergement n'est pas choisi.
// FLASH_MAJ_URL les remplace, pour les essais.
std::vector<std::string> addresses()
{
	char buf[1024];
	size_t n = 0;
	if (getenv_s(&n, buf, sizeof(buf), "FLASH_MAJ_URL") == 0 && n > 1) {
		std::string a(buf);
		if (a.back() != '/')
			a += '/';
		return {a};
	}
	return {};
}

fs::path app_dir()
{
	wchar_t p[MAX_PATH];
	GetModuleFileNameW(nullptr, p, MAX_PATH);
	return fs::path(p).parent_path();
}

fs::path known(REFKNOWNFOLDERID id)
{
	PWSTR s = nullptr;
	fs::path r;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &s)))
		r = s;
	CoTaskMemFree(s);
	return r;
}

fs::path settings_dir() { return known(FOLDERID_RoamingAppData) / L"FLASH"; }
// FLASH_DOSSIER_PLUGIN remplace le dossier du plugin, pour les essais
fs::path plugin_dir()
{
	wchar_t buf[MAX_PATH];
	size_t n = 0;
	if (_wgetenv_s(&n, buf, MAX_PATH, L"FLASH_DOSSIER_PLUGIN") == 0 && n > 1)
		return fs::path(buf);
	return known(FOLDERID_ProgramData) / L"obs-studio" / L"plugins" / L"flash-obs";
}
fs::path plugin_source() { return app_dir() / L"plugin-obs"; }

fs::path from_rel(const std::string &rel) { return fs::path(std::u8string(rel.begin(), rel.end())); }

bool read_file(const fs::path &p, std::vector<uint8_t> &out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

bool write_file(const fs::path &p, const void *data, size_t size)
{
	std::ofstream f(p, std::ios::binary);
	f.write((const char *)data, (std::streamsize)size);
	return (bool)f;
}

bool same_content(const fs::path &a, const fs::path &b)
{
	std::vector<uint8_t> x, y;
	return read_file(a, x) && read_file(b, y) && x == y;
}

fs::path old_name(const fs::path &p) { return fs::path(p.wstring() + L".ancien"); }

// Un antivirus peut garder un fichier tout juste écrit ouvert quelques instants
bool retry(const std::function<bool()> &op)
{
	for (int i = 0; i < 50; i++) {
		if (op())
			return true;
		Sleep(100);
	}
	return false;
}

bool rename_file(const fs::path &from, const fs::path &to)
{
	std::error_code ec;
	fs::rename(from, to, ec);
	return !ec;
}

// Le nouvel exe doit démarrer et charger ses DLL
bool new_exe_starts(const fs::path &dir)
{
	STARTUPINFOW si{sizeof(si)};
	PROCESS_INFORMATION pi{};
	std::wstring cmd = L"\"" + (dir / L"flash.exe").wstring() + L"\" --verifier-maj";
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
		return false;
	const DWORD r = WaitForSingleObject(pi.hProcess, 15000);
	DWORD code = 1;
	if (r == WAIT_OBJECT_0)
		GetExitCodeProcess(pi.hProcess, &code);
	else
		TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return r == WAIT_OBJECT_0 && code == 0;
}

bool install(const std::string &base, const update::Manifest &m, const fs::path &dir, std::string &error)
{
	// 1. Télécharger et vérifier tout ce qui a changé, dans le sous-dossier maj
	const fs::path staging = dir / L"maj";
	std::error_code ec;
	fs::remove_all(staging, ec);
	std::vector<std::string> changed;
	for (const update::File &f : m.files) {
		std::vector<uint8_t> current;
		if (read_file(dir / from_rel(f.path), current) && current.size() == f.size &&
		    update::sha256(current.data(), current.size()) == f.sha256)
			continue;
		std::vector<uint8_t> data;
		if (!update::download(base + update::flat_name(f.path), data, error))
			return false;
		if (data.size() != f.size || update::sha256(data.data(), data.size()) != f.sha256) {
			fs::remove_all(staging, ec);
			error = f.path + " ne correspond pas au manifeste";
			return false;
		}
		const fs::path out = staging / from_rel(f.path);
		fs::create_directories(out.parent_path(), ec);
		if (!write_file(out, data.data(), data.size())) {
			fs::remove_all(staging, ec);
			error = "dossier de FLASH en lecture seule";
			return false;
		}
		changed.push_back(f.path);
	}
	if (changed.empty())
		return true;

	// 2. Les fichiers en service passent de côté (Windows interdit d'écraser un exe lancé,
	//    pas de le renommer), les nouveaux prennent leur place
	std::vector<std::pair<fs::path, bool>> placed;
	for (const std::string &rel : changed) {
		const fs::path target = dir / from_rel(rel);
		const bool existed = fs::exists(target);
		if (existed) {
			fs::remove(old_name(target), ec);
			if (!rename_file(target, old_name(target))) {
				error = "impossible de mettre " + rel + " de côté";
				break;
			}
		}
		fs::create_directories(target.parent_path(), ec);
		if (!retry([&] { return rename_file(staging / from_rel(rel), target); })) {
			if (existed)
				retry([&] { return rename_file(old_name(target), target); });
			error = "impossible de poser " + rel;
			break;
		}
		placed.emplace_back(target, existed);
	}
	fs::remove_all(staging, ec);

	// 3. La nouvelle version doit démarrer, sinon tout revient comme avant
	if (error.empty() && !new_exe_starts(dir))
		error = "la nouvelle version ne démarre pas";
	if (!error.empty()) {
		for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
			const auto &[target, existed] = *it;
			if (existed)
				retry([&] { return rename_file(old_name(target), target); });
			else
				fs::remove(target, ec);
		}
		return false;
	}
	fs::create_directories(settings_dir(), ec);
	write_file(settings_dir() / L"maj-installee.txt", m.version.data(), m.version.size());
	return true;
}

// Copie du plugin : 0 si tout va bien, sinon le code d'erreur de Windows
DWORD copy_plugin()
{
	const fs::path src = plugin_source(), dst = plugin_dir();
	std::error_code ec;
	for (const auto &e : fs::recursive_directory_iterator(src, ec)) {
		if (!e.is_regular_file() || e.path().extension() == L".ancien") // restes d'une mise à jour
			continue;
		const fs::path out = dst / fs::relative(e.path(), src);
		fs::create_directories(out.parent_path(), ec);
		if (ec)
			return ERROR_ACCESS_DENIED;
		if (!CopyFileW(e.path().c_str(), out.c_str(), FALSE))
			return GetLastError();
	}
	return ec ? ERROR_PATH_NOT_FOUND : 0;
}

DWORD delete_plugin()
{
	std::error_code ec;
	fs::remove_all(plugin_dir(), ec);
	return ec ? (DWORD)ec.value() : 0;
}

// Même exe, en administrateur, pour cette seule opération ; attend la fin
std::string elevated(const wchar_t *what)
{
	const std::wstring exe = (app_dir() / L"flash.exe").wstring();
	const std::wstring params = std::wstring(L"--plugin ") + what;
	SHELLEXECUTEINFOW sei{sizeof(sei)};
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
	sei.lpVerb = L"runas";
	sei.lpFile = exe.c_str();
	sei.lpParameters = params.c_str();
	sei.nShow = SW_HIDE;
	if (!ShellExecuteExW(&sei))
		return GetLastError() == ERROR_CANCELLED ? "annulé" : "élévation impossible";
	WaitForSingleObject(sei.hProcess, 60000);
	DWORD code = 1;
	GetExitCodeProcess(sei.hProcess, &code);
	CloseHandle(sei.hProcess);
	return code == 0 ? "" : code == 2 ? "OBS est ouvert" : "échec de la copie";
}

bool denied(DWORD e) { return e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_SHARING_VIOLATION; }

} // namespace

std::string after_launch()
{
	const fs::path dir = app_dir();
	std::error_code ec;
	fs::remove_all(dir / L"maj", ec);
	for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		if (it->path().extension() == L".ancien")
			fs::remove(it->path(), ec);
	const fs::path marker = settings_dir() / L"maj-installee.txt";
	std::vector<uint8_t> v;
	if (!read_file(marker, v))
		return "";
	fs::remove(marker, ec);
	const std::string version(v.begin(), v.end());
	return version == FLASH_VERSION ? version : "";
}

bool has_address() { return !addresses().empty(); }

Result check(std::string &message)
{
	for (const std::string &base : addresses()) {
		std::vector<uint8_t> text, sig;
		std::string error;
		if (!update::download(base + "manifeste.txt", text, error, 1 << 20) ||
		    !update::download(base + "manifeste.sig", sig, error, 4096))
			continue;
		update::Manifest m;
		if (!update::parse_manifest(std::string(text.begin(), text.end()), std::string(sig.begin(), sig.end()), kPublicKey, m,
					    error)) {
			message = error;
			FLOG_W("Mise à jour : %s", error.c_str());
			return Result::Failed;
		}
		if (!update::newer(m.version, FLASH_VERSION))
			return Result::Nothing;
		if (!install(base, m, app_dir(), error)) {
			message = error;
			FLOG_W("Mise à jour vers %s : %s", m.version.c_str(), error.c_str());
			return Result::Failed;
		}
		message = m.version;
		FLOG_I("Mise à jour %s installée, active au prochain lancement", m.version.c_str());
		return Result::Installed;
	}
	return Result::Nothing;
}

Plugin plugin_state()
{
	const fs::path src = plugin_source(), dst = plugin_dir();
	std::error_code ec;
	if (!fs::exists(src / L"bin" / L"64bit" / L"flash-obs.dll", ec))
		return Plugin::NoSource;
	if (!fs::exists(dst / L"bin" / L"64bit" / L"flash-obs.dll", ec))
		return Plugin::Absent;
	for (const auto &e : fs::recursive_directory_iterator(src, ec))
		if (e.is_regular_file() && e.path().extension() != L".ancien" && !same_content(e.path(), dst / fs::relative(e.path(), src)))
			return Plugin::Different;
	return Plugin::UpToDate;
}

bool obs_running()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return false;
	PROCESSENTRY32W e{sizeof(e)};
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
		found = _wcsicmp(e.szExeFile, L"obs64.exe") == 0;
	CloseHandle(snap);
	return found;
}

std::string install_plugin()
{
	if (obs_running())
		return "Fermez OBS d'abord : il garde le plugin ouvert.";
	const DWORD e = copy_plugin();
	if (e == 0)
		return "";
	if (!denied(e))
		return "Copie du plugin impossible (" + std::to_string(e) + ")";
	const std::string r = elevated(L"installer");
	return r == "annulé" ? "Installation annulée." : r.empty() ? "" : "Plugin non installé : " + r + ".";
}

std::string remove_plugin()
{
	std::error_code ec;
	if (!fs::exists(plugin_dir(), ec))
		return "";
	if (obs_running())
		return "Fermez OBS d'abord : il garde le plugin ouvert.";
	const DWORD e = delete_plugin();
	if (e == 0)
		return "";
	const std::string r = elevated(L"retirer");
	return r == "annulé" ? "Retrait annulé." : r.empty() ? "" : "Plugin non retiré : " + r + ".";
}

bool plugin_offered()
{
	std::error_code ec;
	return fs::exists(settings_dir() / L"plugin-propose.txt", ec);
}

void set_plugin_offered()
{
	std::error_code ec;
	fs::create_directories(settings_dir(), ec);
	write_file(settings_dir() / L"plugin-propose.txt", "1", 1);
}

void remove_settings()
{
	RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"FLASH");
	// Réglages et appairages de l'appli, cache de l'interface, appairages du plugin dans OBS.
	// Les processus de l'interface peuvent garder leur cache ouvert quelques instants.
	const fs::path dirs[] = {settings_dir(), known(FOLDERID_LocalAppData) / L"FLASH",
				 known(FOLDERID_RoamingAppData) / L"obs-studio" / L"plugin_config" / L"flash-obs"};
	for (const fs::path &d : dirs) {
		retry([&] {
			std::error_code ec;
			fs::remove_all(d, ec);
			return !ec;
		});
	}
}

bool installed_edition()
{
	std::error_code ec;
	return fs::exists(app_dir() / L"unins000.exe", ec);
}

void run_uninstaller()
{
	const std::wstring exe = (app_dir() / L"unins000.exe").wstring();
	ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int run_command(int argc, char **argv)
{
	if (argc < 2)
		return -1;
	const std::string a = argv[1];
	if (a == "--verifier-maj")
		return 0;
	// Recherche immédiate d'une mise à jour, en ligne de commande
	if (a == "--chercher-maj") {
		std::string message;
		const Result r = check(message);
		std::printf("%s\n", r == Result::Installed ? ("installée : " + message).c_str()
				    : r == Result::Failed  ? ("échec : " + message).c_str()
							   : (has_address() ? "rien de neuf" : "aucune adresse de publication"));
		return r == Result::Failed ? 1 : 0;
	}
	// Désinstalleur : retrait du plugin, avec élévation seulement si Windows l'exige
	if (a == "--plugin-retrait")
		return remove_plugin().empty() ? 0 : 1;
	if (a == "--plugin" && argc >= 3) {
		// Côté élevé : rien d'autre que la copie ou le retrait du plugin
		const std::string what = argv[2];
		if (obs_running())
			return 2;
		if (what == "installer")
			return copy_plugin() == 0 ? 0 : 1;
		if (what == "retirer")
			return delete_plugin() == 0 ? 0 : 1;
		return 1;
	}
	return -1;
}

} // namespace updater
