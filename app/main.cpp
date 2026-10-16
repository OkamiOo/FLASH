// FLASH : envoi. Pour l'instant, mode mire uniquement (étape de mesure en boucle locale).

#include "mire_source.h"
#include "opus_encoder.h"

#include <flash/clock.h>
#include <flash/log.h>
#include <flash/mire.h>
#include <flash/receiver.h>
#include <flash/srt_link.h>
#include <flash/ts.h>
#include <flash/video.h>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace flash;

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI on_console_event(DWORD)
{
	g_running = false;
	return TRUE;
}

struct Args {
	bool mire = false;
	std::string address = "127.0.0.1";
	uint16_t port = 9710;
	uint32_t bitrate_kbps = 25000;
	int latency_ms = 20;
	bool hevc = false;
	int duration_s = 0;
};

bool parse_args(int argc, char **argv, Args &a)
{
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
		if (arg == "--mire")
			a.mire = true;
		else if (arg == "--adresse")
			a.address = next();
		else if (arg == "--port")
			a.port = (uint16_t)std::atoi(next());
		else if (arg == "--debit")
			a.bitrate_kbps = (uint32_t)std::atoi(next());
		else if (arg == "--latence")
			a.latency_ms = std::atoi(next());
		else if (arg == "--hevc")
			a.hevc = true;
		else if (arg == "--duree")
			a.duration_s = std::atoi(next());
		else
			return false;
	}
	return a.mire;
}

std::string host_name()
{
	char name[256] = "PC";
	DWORD size = sizeof(name);
	GetComputerNameA(name, &size);
	return name;
}

int run_mire(const Args &args)
{
	enable_precise_timers();
	srt_init(true);

	std::string gpu;
	ComPtr<ID3D11Device> device = create_d3d11_device(&gpu);
	if (!device)
		return 1;
	FLOG_I("GPU : %s", gpu.c_str());

	MireSource mire;
	if (!mire.init(device.Get()))
		return 1;

	VideoEncoderConfig vc;
	vc.width = mire::kWidth;
	vc.height = mire::kHeight;
	vc.fps_num = mire::kFps;
	vc.bitrate_kbps = args.bitrate_kbps;
	vc.codec = args.hevc ? Codec::HEVC : Codec::H264;
	std::unique_ptr<VideoEncoder> encoder = create_nvenc_encoder(device.Get(), vc);
	if (!encoder) {
		FLOG_E("NVENC indisponible");
		return 1;
	}

	AudioEncoder opus;
	if (!opus.open(2, 192000))
		return 1;

	SrtServer server;
	TsMuxer mux([&server](const uint8_t *data, size_t size) { server.send(data, size); });
	mux.set_tracks({{ts::kPidVideo0, vc.codec}, {ts::kPidAudio0, Codec::Opus, 2}});

	std::atomic<bool> want_idr{true};
	std::atomic<uint64_t> last_idr_request{0};

	SrtOptions so;
	so.address = args.address;
	so.port = args.port;
	so.latency_ms = args.latency_ms;
	const bool listening = server.start(
		so,
		[&](int) {
			want_idr = true;
			mux.write_tables();
		},
		[&](int, const uint8_t *data, size_t size) {
			// Demande d'image clé : une toutes les 250 ms au plus
			const uint64_t now = now_ns();
			if (size >= 1 && data[0] == control::kKeyframeRequest && now - last_idr_request > 250'000'000) {
				last_idr_request = now;
				want_idr = true;
			}
		});
	if (!listening)
		return 1;

	const std::string meta = "{\"v\":1,\"nom\":\"" + host_name() +
				 "\",\"pistes\":[{\"pid\":256,\"type\":\"video\",\"nom\":\"Mire\"},"
				 "{\"pid\":512,\"type\":\"audio\",\"nom\":\"Bip\"}]}";

	const uint64_t t0 = now_ns() + 200'000'000ULL;
	const uint64_t stop_at = args.duration_s > 0 ? t0 + (uint64_t)args.duration_s * 1'000'000'000ULL : 0;

	std::thread audio([&] {
		std::vector<float> buf((size_t)opus.frame_size() * 2);
		std::vector<uint8_t> packet;
		const double w = 2.0 * 3.14159265358979323846 * mire::kBeepHz / mire::kSampleRate;

		for (uint64_t m = 0; g_running; m++) {
			const uint64_t first = m * (uint64_t)opus.frame_size();
			sleep_until_ns(t0 + (first + opus.frame_size()) * 1'000'000'000ULL / mire::kSampleRate);

			for (int i = 0; i < opus.frame_size(); i++) {
				const uint64_t phase = (first + i) % mire::kSampleRate;
				const float v = phase < mire::kBeepSamples ? (float)(0.5 * std::sin(w * (double)phase)) : 0.0f;
				buf[2 * i] = buf[2 * i + 1] = v;
			}
			if (!opus.encode(buf.data(), packet))
				continue;

			// Le PTS désigne le premier échantillon décodé : l'encodeur décale de son retard
			const int64_t pts_samples = (int64_t)first - opus.lookahead();
			const uint64_t pts = t0 + (uint64_t)(pts_samples * 1'000'000'000LL / (int64_t)mire::kSampleRate);
			mux.write_audio(ts::kPidAudio0, pts, packet.data(), packet.size());

			if (m % 2 == 0)
				mux.write_pcr(now_ns());
			if (m % 10 == 0)
				mux.write_tables();
			if (m % 100 == 0)
				mux.write_meta(meta);
		}
	});

	FLOG_I("Mire envoyée sur srt://%s:%u (Ctrl+C pour arrêter)", args.address.c_str(), args.port);

	EncodedFrame frame;
	uint64_t enc_total = 0, enc_max = 0, bytes = 0, late = 0;
	uint32_t frames = 0;
	uint64_t report_at = t0 + 5'000'000'000ULL;

	for (uint32_t n = 0; g_running; n++) {
		const uint64_t t = t0 + (uint64_t)n * 1'000'000'000ULL / mire::kFps;
		if (stop_at && t >= stop_at)
			break;
		sleep_until_ns(t);

		ID3D11Texture2D *tex = mire.render(n, t);
		if (!tex)
			continue;

		const uint64_t e0 = now_ns();
		if (!encoder->encode(tex, want_idr.exchange(false), frame))
			continue;
		const uint64_t e1 = now_ns();
		mux.write_video(ts::kPidVideo0, t, frame.data.data(), frame.data.size(), frame.keyframe);

		enc_total += e1 - e0;
		enc_max = e1 - e0 > enc_max ? e1 - e0 : enc_max;
		bytes += frame.data.size();
		frames++;
		if ((int64_t)(e0 - t) > 1'000'000)
			late++;

		if (e1 >= report_at) {
			FLOG_I("%u images, encodage %.2f ms (max %.2f), %.1f Mb/s, %llu en retard, %zu récepteur(s)", frames,
			       enc_total / 1e6 / frames, enc_max / 1e6, bytes * 8 / 5e6, (unsigned long long)late,
			       server.client_count());
			enc_total = enc_max = bytes = late = 0;
			frames = 0;
			report_at += 5'000'000'000ULL;
		}
	}

	g_running = false;
	audio.join();
	server.stop();
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCtrlHandler(on_console_event, TRUE);

	Args args;
	if (!parse_args(argc, argv, args)) {
		std::printf("Usage : flash --mire [--adresse 127.0.0.1] [--port 9710] [--debit 25000] [--latence 20] "
			    "[--hevc] [--duree secondes]\n");
		return 2;
	}
	const int code = run_mire(args);
	shutdown_log();
	return code;
}
