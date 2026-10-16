// FLASH : envoi en ligne de commande, en attendant l'interface.

#include "banc.h"

#include <flash/clock.h>
#include <flash/inputs.h>
#include <flash/log.h>
#include <flash/sender.h>

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
  flash --liste                     écrans, fenêtres, applis qui jouent du son, micros
  flash [sources] [réglages]        envoie les sources choisies
  flash --banc [--sortie nom] [--ecran N]   fenêtre d'essai (image + bips) à capturer

Sources (autant que voulu) :
  --mire                 mire de mesure et son bip
  --ecran N              écran N (1 = principal)
  --fenetre texte        fenêtre dont l'exécutable ou le titre correspond
  --appli nom.exe        son d'une application
  --systeme              son de tout le système, sauf FLASH
  --micro [nom]          micro (par défaut celui de Windows)

Réglages :
  --adresse ip           adresse d'écoute (127.0.0.1 par défaut)
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
	bool banc = false;
	int banc_screen = 0; // 0 = dernier écran
	std::string output;
	std::vector<std::unique_ptr<VideoInput>> video;
	std::vector<std::unique_ptr<AudioInput>> audio;
	SenderOptions options;
	uint32_t max_w = 1920, max_h = 1080;
	bool cursor = true;
	int duration_s = 0;
};

bool parse_args(int argc, char **argv, Args &a)
{
	// Les sources se créent après lecture de tous les réglages (taille, curseur)
	struct Pending {
		std::string kind, value;
	};
	std::vector<Pending> sources;

	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		auto optional = [&]() -> std::string {
			return (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "";
		};
		if (arg == "--liste")
			a.list = true;
		else if (arg == "--banc")
			a.banc = true;
		else if (arg == "--sortie")
			a.output = next();
		else if (arg == "--mire" || arg == "--systeme")
			sources.push_back({arg, ""});
		else if (arg == "--ecran" || arg == "--fenetre" || arg == "--appli")
			sources.push_back({arg, next()});
		else if (arg == "--micro")
			sources.push_back({arg, optional()});
		else if (arg == "--adresse")
			a.options.srt.address = next();
		else if (arg == "--port")
			a.options.srt.port = (uint16_t)std::atoi(next().c_str());
		else if (arg == "--debit")
			a.options.video_kbps = (uint32_t)std::atoi(next().c_str());
		else if (arg == "--latence")
			a.options.srt.latency_ms = std::atoi(next().c_str());
		else if (arg == "--taille") {
			const std::string v = next();
			if (std::sscanf(v.c_str(), "%ux%u", &a.max_w, &a.max_h) != 2)
				return false;
		} else if (arg == "--hevc")
			a.options.codec = Codec::HEVC;
		else if (arg == "--encodeur")
			a.options.encoder = next();
		else if (arg == "--sans-curseur")
			a.cursor = false;
		else if (arg == "--duree")
			a.duration_s = std::atoi(next().c_str());
		else
			return false;
	}

	// Avec --banc, --ecran désigne l'écran où ouvrir la fenêtre d'essai
	if (a.banc) {
		for (const Pending &s : sources)
			if (s.kind == "--ecran")
				a.banc_screen = std::atoi(s.value.c_str());
		return true;
	}

	// La mire démarre sur une seconde pleine de l'horloge commune, comme le banc
	const uint64_t t0 = (now_ns() / 1'000'000'000ULL + 1) * 1'000'000'000ULL;
	for (const Pending &s : sources) {
		if (s.kind == "--mire") {
			a.video.push_back(make_mire_input(t0));
			a.audio.push_back(make_beep_input(t0));
		} else if (s.kind == "--ecran") {
			a.video.push_back(make_screen_input(std::atoi(s.value.c_str()), a.max_w, a.max_h, a.cursor));
		} else if (s.kind == "--fenetre") {
			a.video.push_back(make_window_input(s.value, a.max_w, a.max_h, a.cursor));
		} else if (s.kind == "--appli") {
			a.audio.push_back(make_app_audio_input(s.value));
		} else if (s.kind == "--systeme") {
			a.audio.push_back(make_system_audio_input());
		} else if (s.kind == "--micro") {
			a.audio.push_back(make_mic_input(s.value));
		}
	}
	return a.list || a.banc || !a.video.empty() || !a.audio.empty();
}

std::string host_name()
{
	char name[256] = "PC";
	DWORD size = sizeof(name);
	GetComputerNameA(name, &size);
	return name;
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

int run_sender(Args &args)
{
	enable_precise_timers();

	std::string gpu;
	ComPtr<ID3D11Device> device = create_d3d11_device(&gpu);
	if (!device)
		return 1;
	FLOG_I("GPU : %s", gpu.c_str());

	args.options.host_name = host_name();
	Sender sender;
	if (!sender.start(args.options, device, std::move(args.video), std::move(args.audio)))
		return 1;
	FLOG_I("Envoi sur srt://%s:%u (Ctrl+C pour arrêter)", args.options.srt.address.c_str(), args.options.srt.port);

	const uint64_t start = now_ns();
	uint64_t report = start + 5'000'000'000ULL;
	while (g_running) {
		Sleep(100);
		const uint64_t now = now_ns();
		if (args.duration_s > 0 && now - start >= (uint64_t)args.duration_s * 1'000'000'000ULL)
			break;
		if (now < report)
			continue;
		report += 5'000'000'000ULL;
		std::string line = std::to_string(sender.client_count()) + " récepteur(s)";
		for (const TrackStats &s : sender.stats()) {
			char buf[160];
			if (s.video)
				std::snprintf(buf, sizeof(buf), " | %s : %.0f i/s (%.0f capturées, %llu clés), %.1f Mb/s, encodage %.2f ms (max %.2f)",
					      s.name.c_str(), s.frames / 5.0, s.captured / 5.0, (unsigned long long)s.keyframes,
					      s.bytes * 8 / 5e6, s.encode_ms_avg, s.encode_ms_max);
			else
				std::snprintf(buf, sizeof(buf), " | %s : %.0f kb/s", s.name.c_str(), s.bytes * 8 / 5e3);
			line += s.active ? buf : " | " + s.name + " : en pause";
		}
		FLOG_I("%s", line.c_str());
	}
	sender.stop();
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCtrlHandler(on_console_event, TRUE);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	Args args;
	if (!parse_args(argc, argv, args)) {
		std::printf("%s", kUsage);
		return 2;
	}

	int code = 0;
	if (args.list)
		print_lists();
	else if (args.banc)
		code = run_banc(args.output, args.banc_screen, args.duration_s);
	else
		code = run_sender(args);
	shutdown_log();
	return code;
}
