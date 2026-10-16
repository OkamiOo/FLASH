// Sortie son WASAPI partagée, pilotée par événement. Quand la sortie travaille déjà en
// 48 kHz flottant, on demande à Windows sa plus petite période (IAudioClient3) ; sinon Windows
// convertit notre 48 kHz stéréo, avec sa période habituelle de 10 ms.

#include "wasapi.h"
#include "flash/audio_output.h"
#include "flash/clock.h"
#include "flash/inputs.h"
#include "flash/log.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>

#include <algorithm>
#include <vector>

namespace flash {

namespace {

constexpr uint32_t kRate = 48000;

bool is_float48(const WAVEFORMATEX *f)
{
	if (f->nSamplesPerSec != kRate || f->wBitsPerSample != 32 || f->nChannels < 2)
		return false;
	if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
	       ((const WAVEFORMATEXTENSIBLE *)f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

} // namespace

AudioOutput::~AudioOutput()
{
	stop();
}

bool AudioOutput::start(const std::string &device, Fill fill)
{
	device_ = device;
	fill_ = std::move(fill);
	running_ = true;
	thread_ = std::thread(&AudioOutput::run, this);
	while (!ready_ && !failed_)
		Sleep(1);
	return !failed_;
}

void AudioOutput::stop()
{
	if (running_.exchange(false) && thread_.joinable())
		thread_.join();
}

void AudioOutput::run()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	DWORD task = 0;
	HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);

	ComPtr<IMMDevice> dev = find_audio_device(eRender, to_wide(device_), &name_);
	ComPtr<IAudioClient> client;
	if (!dev || FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)client.GetAddressOf()))) {
		FLOG_E("Sortie son « %s » introuvable", device_.c_str());
		failed_ = true;
		CoUninitialize();
		return;
	}

	// Petite période si la sortie est déjà en 48 kHz flottant
	uint32_t channels = 2;
	uint32_t period = kRate / 100;
	bool initialized = false;
	ComPtr<IAudioClient3> client3;
	WAVEFORMATEX *mix = nullptr;
	if (SUCCEEDED(client.As(&client3)) && SUCCEEDED(client3->GetMixFormat(&mix)) && is_float48(mix)) {
		UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
		if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &def, &fundamental, &minimum, &maximum)) &&
		    SUCCEEDED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum, mix, nullptr))) {
			channels = mix->nChannels;
			period = minimum;
			initialized = true;
		}
	}
	CoTaskMemFree(mix);
	if (!initialized) {
		WAVEFORMATEX fmt{};
		fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		fmt.nChannels = 2;
		fmt.nSamplesPerSec = kRate;
		fmt.wBitsPerSample = 32;
		fmt.nBlockAlign = 8;
		fmt.nAvgBytesPerSec = kRate * 8;
		const HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
						      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
							      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
						      100000, 0, &fmt, nullptr);
		if (FAILED(hr)) {
			FLOG_E("Ouverture de la sortie son « %s » impossible (0x%08lx)", name_.c_str(), (unsigned long)hr);
			failed_ = true;
			CoUninitialize();
			return;
		}
	}

	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	client->SetEventHandle(event);
	ComPtr<IAudioRenderClient> render;
	ComPtr<IAudioClock> clock;
	client->GetService(IID_PPV_ARGS(&render));
	client->GetService(IID_PPV_ARGS(&clock));
	UINT64 freq = 1;
	clock->GetFrequency(&freq);
	UINT32 buffer = 0;
	client->GetBufferSize(&buffer);
	// On garde deux périodes d'avance : assez pour ne jamais manquer, pas plus
	const UINT32 keep = std::min(buffer, period * 2);
	REFERENCE_TIME stream_latency = 0;
	client->GetStreamLatency(&stream_latency);
	FLOG_I("Sortie son « %s » : période %.2f ms, tampon %.1f ms, latence annoncée %.1f ms%s", name_.c_str(),
	       period * 1000.0 / kRate, buffer * 1000.0 / kRate, stream_latency / 10000.0,
	       initialized ? " (petite période)" : "");
	bool reported = false;
	const uint64_t started = now_ns();

	std::vector<float> stereo;
	uint64_t written = 0;
	int64_t origin = 0; // heure de restitution de l'échantillon 0
	bool have_origin = false;
	uint64_t worst = 0, worst_since = now_ns();
	client->Start();
	ready_ = true;

	while (running_) {
		WaitForSingleObject(event, 100);
		UINT64 pos = 0, qpc = 0;
		if (SUCCEEDED(clock->GetPosition(&pos, &qpc)) && pos > 0) {
			const int64_t o = (int64_t)(qpc * 100) - (int64_t)(pos * 1'000'000'000ULL / freq);
			origin = have_origin ? origin + (o - origin) / 16 : o;
			have_origin = true;
		}
		UINT32 padding = 0;
		client->GetCurrentPadding(&padding);
		if (padding >= keep)
			continue;
		const UINT32 frames = keep - padding;
		BYTE *data = nullptr;
		if (FAILED(render->GetBuffer(frames, &data)))
			continue;

		// Sans horloge du périphérique (tout début), on estime : la file actuelle puis la suite
		const uint64_t now = now_ns();
		const uint64_t first = have_origin ? (uint64_t)(origin + (int64_t)(written * 1'000'000'000ULL / kRate))
						   : now + (uint64_t)padding * 1'000'000'000ULL / kRate;
		stereo.assign((size_t)frames * 2, 0.0f);
		fill_(stereo.data(), frames, first);
		float *out = (float *)data;
		if (channels == 2) {
			std::copy(stereo.begin(), stereo.end(), out);
		} else {
			std::fill(out, out + (size_t)frames * channels, 0.0f);
			for (UINT32 i = 0; i < frames; i++) {
				out[(size_t)i * channels] = stereo[(size_t)i * 2];
				out[(size_t)i * channels + 1] = stereo[(size_t)i * 2 + 1];
			}
		}
		render->ReleaseBuffer(frames, 0);
		written += frames;

		// Latence : de maintenant à la restitution du dernier échantillon écrit
		const uint64_t last = first + (uint64_t)frames * 1'000'000'000ULL / kRate;
		const uint64_t lat = last > now ? last - now : 0;
		worst = std::max(worst, lat);
		if (now - worst_since > 2'000'000'000ULL) {
			latency_ = worst;
			worst = 0;
			worst_since = now;
		} else if (worst > latency_) {
			latency_ = worst;
		}
		if (!reported && now - started > 3'000'000'000ULL) {
			reported = true;
			FLOG_I("Sortie son « %s » : %.1f ms entre l'écriture et l'écoute", name_.c_str(), latency_ / 1e6);
		}
	}

	client->Stop();
	CloseHandle(event);
	if (mm)
		AvRevertMmThreadCharacteristics(mm);
	CoUninitialize();
}

} // namespace flash
