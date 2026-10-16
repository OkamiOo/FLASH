// FLASH : sans argument, l'interface ; avec des arguments, la ligne de commande (essais, scripts).

#include "banc.h"
#include "engine.h"
#include "ui.h"

#include <flash/clock.h>
#include <flash/discovery.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/measure.h>
#include <flash/pairing.h>
#include <flash/security.h>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace flash;

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI on_console_event(DWORD)
{
	g_running = false;
	return TRUE;
}

const char *kUsage = R"(Usage :
  flash                             interface
  flash --liste                     écrans, fenêtres, applis qui jouent du son, micros
  flash --chercher                  émetteurs FLASH du réseau
  flash --appairer adresse code     appaire ce PC (pour regarder) avec l'émetteur à cette adresse
  flash [sources] [réglages]        envoie les sources choisies
  flash --voir émetteur [options]   ouvre une visionneuse (avec ou sans envoi)
  flash --banc [--sortie nom] [--ecran N]   fenêtre d'essai (image + bips) à capturer

Visionneuse (émetteur : nom, identifiant, adresse IP, ou « moi » pour ce PC) :
  --piste nom            piste vidéo (par défaut la première ; « - » : aucune)
  --son nom              piste son, autant que voulu (« tous » : toutes)
  --sortie nom           sortie son (par défaut celle de Windows)
  --plein-ecran          fenêtre en plein écran
  --ecran-vue N          écran où ouvrir la fenêtre

Sources (autant que voulu) :
  --mire                 mire de mesure et son bip
  --ecran N              écran N (1 = principal)
  --fenetre texte        fenêtre dont l'exécutable ou le titre correspond
  --appli nom.exe        son d'une application
  --systeme              son de tout le système, sauf FLASH
  --micro [nom]          micro (par défaut celui de Windows)

Réglages :
  --reseau               accessible depuis le réseau (sinon ce PC seulement)
  --libre                accepte les récepteurs sans appairage
  --port N               port SRT (9710)
  --debit kbps           débit vidéo par piste (25000)
  --latence ms           latence SRT (20)
  --taille LxH           taille maximale envoyée (1920x1080)
  --hevc                 HEVC au lieu de H.264
  --encodeur auto|nvenc|mf|logiciel   encodeur vidéo (auto : NVENC, sinon Media Foundation)
  --sans-curseur         ne pas capturer le curseur
  --duree s              s'arrêter au bout de s secondes
)";

struct Args {
	bool list = false;
	bool search = false;
	bool banc = false;
	int banc_screen = 0; // 0 = dernier écran
	std::string output;
	std::string pair_address, pair_code;
	std::vector<SourceChoice> sources;
	EngineSettings settings;
	int duration_s = 0;
	std::string watch;
	WatchRequest view;
};

bool parse_args(int argc, char **argv, Args &a)
{
	a.settings.address = "127.0.0.1";
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		auto optional = [&]() -> std::string {
			return (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "";
		};
		if (arg == "--liste")
			a.list = true;
		else if (arg == "--chercher")
			a.search = true;
		else if (arg == "--appairer") {
			a.pair_address = next();
			a.pair_code = next();
		} else if (arg == "--banc")
			a.banc = true;
		else if (arg == "--sortie")
			a.output = next();
		else if (arg == "--mire")
			a.sources.push_back({"mire", ""});
		else if (arg == "--systeme")
			a.sources.push_back({"systeme", ""});
		else if (arg == "--ecran") {
			const std::string v = next();
			a.sources.push_back({"ecran", v});
			a.banc_screen = std::atoi(v.c_str());
		} else if (arg == "--fenetre")
			a.sources.push_back({"fenetre", next()});
		else if (arg == "--appli")
			a.sources.push_back({"appli", next()});
		else if (arg == "--micro")
			a.sources.push_back({"micro", optional()});
		else if (arg == "--reseau")
			a.settings.address = "0.0.0.0";
		else if (arg == "--libre")
			a.settings.open = true;
		else if (arg == "--port")
			a.settings.port = (uint16_t)std::atoi(next().c_str());
		else if (arg == "--debit")
			a.settings.video_kbps = (uint32_t)std::atoi(next().c_str());
		else if (arg == "--latence")
			a.settings.latency_ms = std::atoi(next().c_str());
		else if (arg == "--taille") {
			if (std::sscanf(next().c_str(), "%ux%u", &a.settings.max_w, &a.settings.max_h) != 2)
				return false;
		} else if (arg == "--hevc")
			a.settings.hevc = true;
		else if (arg == "--encodeur")
			a.settings.encoder = next();
		else if (arg == "--sans-curseur")
			a.settings.cursor = false;
		else if (arg == "--duree")
			a.duration_s = std::atoi(next().c_str());
		else if (arg == "--voir")
			a.watch = next();
		else if (arg == "--piste")
			a.view.video = next();
		else if (arg == "--son") {
			const std::string s = next();
			a.view.audio.push_back(s == "tous" ? "*" : s);
		} else if (arg == "--plein-ecran")
			a.view.fullscreen = true;
		else if (arg == "--ecran-vue")
			a.view.screen = std::atoi(next().c_str());
		else
			return false;
	}
	a.view.output = a.output;
	return a.list || a.search || a.banc || !a.pair_address.empty() || !a.sources.empty() || !a.watch.empty();
}

void print_lists()
{
	std::printf("Écrans :\n");
	for (const ScreenInfo &s : list_screens())
		std::printf("  %d : %dx%d à %d Hz en (%d, %d), %s%s\n", s.index, s.width, s.height, s.hz, s.x, s.y,
			    s.adapter.c_str(), s.primary ? " (principal)" : "");
	std::printf("Fenêtres :\n");
	for (const WindowInfo &w : list_windows())
		std::printf("  %-28s %s\n", w.exe.c_str(), w.title.c_str());
	std::printf("Applis avec une session audio :\n");
	for (const AudioAppInfo &a : list_audio_apps())
		std::printf("  %-28s %s\n", a.exe.c_str(), a.playing ? "joue du son" : "silencieuse");
	std::printf("Micros :\n");
	for (const std::string &m : list_microphones())
		std::printf("  %s\n", m.c_str());
}

void print_senders()
{
	const auto senders = discover_senders(800);
	if (senders.empty())
		std::printf("Aucun émetteur FLASH trouvé.\n");
	for (const SenderAnnounce &s : senders) {
		std::printf("%s (%s) %s:%u%s\n", s.name.c_str(), s.id.c_str(), s.address.c_str(), s.srt_port,
			    s.open ? ", ouvert" : "");
		for (const AnnouncedTrack &t : s.tracks)
			std::printf("  %s « %s »\n", t.video ? "vidéo" : "son", t.name.c_str());
	}
}

int run_pairing(const Args &args)
{
	Engine engine;
	engine.start(false, false);
	const PairingResult r = engine.pair(args.pair_address, kPairingPort, args.pair_code);
	if (!r.ok) {
		std::printf("Échec : %s\n", r.error.c_str());
		return 1;
	}
	std::printf("Appairé avec « %s » (%s)\n", r.sender.name.c_str(), r.sender.id.c_str());
	return 0;
}

// Émetteur désigné en ligne de commande : identifiant, nom du PC, adresse IP, ou « moi »
bool resolve_watch(Engine &engine, const std::string &target, WatchRequest &r)
{
	if (target == "moi") {
		r.sender = "moi";
		return true;
	}
	for (const SenderAnnounce &s : engine.discover(true)) {
		if (s.id == target || _stricmp(s.name.c_str(), target.c_str()) == 0 || s.address == target) {
			r.sender = s.id;
			return true;
		}
	}
	if (target.find('.') != std::string::npos) {
		r.address = target; // émetteur ouvert, hors de portée de la découverte
		return true;
	}
	r.sender = target; // appairé, peut-être pas encore lancé
	return true;
}

std::string view_line(const nlohmann::json &v)
{
	char buf[200];
	std::snprintf(buf, sizeof(buf), " | visionneuse : %s, %.0f i/s, %.1f Mb/s", v.value("etat", "").c_str(),
		      v.value("ips", 0.0), v.value("debit", 0.0));
	std::string line = buf;
	if (v.contains("latence_ms")) {
		std::snprintf(buf, sizeof(buf), ", latence %.1f ms", v["latence_ms"].get<double>());
		line += buf;
	}
	if (v.value("sautees", 0) > 0)
		line += ", " + std::to_string(v["sautees"].get<int>()) + " sautée(s)";
	if (!v.value("message", "").empty())
		line += " (" + v["message"].get<std::string>() + ")";
	return line;
}

int run_console(const Args &args)
{
	enable_precise_timers();
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	Engine engine;
	const EngineSettings &s = args.settings;
	engine.preset(s);
	const bool sending = !args.sources.empty();
	if (!engine.start(false, sending))
		return 1;
	if (sending) {
		engine.set_sources(args.sources);
		FLOG_I("« %s » : envoi sur %s:%u, code d'invitation %s (Ctrl+C pour arrêter)", engine.name().c_str(),
		       s.address.c_str(), s.port, engine.code().c_str());
	}
	if (!args.watch.empty()) {
		WatchRequest r = args.view;
		r.background = true; // en essai, la fenêtre ne prend pas la main sur le clavier
		std::string error;
		if (!resolve_watch(engine, args.watch, r) || !engine.watch(r, error)) {
			std::printf("Visionneuse impossible : %s\n", error.c_str());
			return 1;
		}
	}

	const uint64_t start = now_ns();
	uint64_t report = start + 5'000'000'000ULL;
	engine.status(); // départ des compteurs
	while (g_running) {
		Sleep(100);
		const uint64_t now = now_ns();
		if (args.duration_s > 0 && now - start >= (uint64_t)args.duration_s * 1'000'000'000ULL)
			break;
		if (!args.watch.empty() && engine.views() == 0)
			break; // fenêtre fermée
		if (now < report)
			continue;
		report += 5'000'000'000ULL;
		const nlohmann::json st = engine.status();
		std::string line = std::to_string(st["connectes"].get<size_t>()) + " récepteur(s)";
		for (const auto &v : st["vues"])
			line += view_line(v);
		for (const auto &t : st["pistes"]) {
			char buf[160];
			if (!t["active"].get<bool>())
				std::snprintf(buf, sizeof(buf), " | %s : en pause", t["nom"].get<std::string>().c_str());
			else if (t["video"].get<bool>())
				std::snprintf(buf, sizeof(buf), " | %s : %.0f i/s, %.1f Mb/s, encodage %.2f ms",
					      t["nom"].get<std::string>().c_str(), t["ips"].get<double>(),
					      t["kbps"].get<double>() / 1000, t["encodage_ms"].get<double>());
			else
				std::snprintf(buf, sizeof(buf), " | %s : %.0f kb/s", t["nom"].get<std::string>().c_str(),
					      t["kbps"].get<double>());
			line += buf;
		}
		FLOG_I("%s", line.c_str());
	}
	engine.stop();
	measure::shutdown();
	return 0;
}

// Appli fenêtrée : en ligne de commande, on écrit dans la console qui l'a lancée
void attach_console()
{
	if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN)
		return; // sorties déjà redirigées (script)
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		std::freopen("CONOUT$", "w", stdout);
		std::freopen("CONOUT$", "w", stderr);
	}
}

} // namespace

int main(int argc, char **argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	if (argc <= 1 || std::string(argv[1]) == "--demarrage") {
		const int code = run_ui(argc > 1);
		shutdown_log();
		return code;
	}

	attach_console();
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCtrlHandler(on_console_event, TRUE);

	Args args;
	if (!parse_args(argc, argv, args)) {
		std::printf("%s", kUsage);
		return 2;
	}

	int code = 0;
	if (args.list)
		print_lists();
	else if (args.search)
		print_senders();
	else if (!args.pair_address.empty())
		code = run_pairing(args);
	else if (args.banc)
		code = run_banc(args.output, args.banc_screen, args.duration_s);
	else
		code = run_console(args);
	shutdown_log();
	return code;
}
