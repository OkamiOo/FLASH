#include "flash/clock.h"
#include "flash/inputs.h"
#include "flash/log.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <vector>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace flash {

namespace {

constexpr uint32_t kRate = 48000;

std::wstring lower(std::wstring s)
{
	CharLowerBuffW(s.data(), (DWORD)s.size());
	return s;
}

std::wstring exe_of_pid(DWORD pid)
{
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

// Processus racine d'une appli : le premier processus de ce nom dont le parent porte un
// autre nom (Discord, Chrome… lancent des dizaines de sous-processus du même nom).
DWORD find_root_process(const std::wstring &exe)
{
	std::wstring want = lower(exe);
	if (want.size() < 4 || want.substr(want.size() - 4) != L".exe")
		want += L".exe";

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	std::map<DWORD, std::pair<DWORD, std::wstring>> procs; // pid -> (parent, nom)
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
		procs[pe.th32ProcessID] = {pe.th32ParentProcessID, lower(pe.szExeFile)};
	CloseHandle(snap);

	DWORD fallback = 0;
	for (const auto &[pid, info] : procs) {
		if (info.second != want)
			continue;
		auto parent = procs.find(info.first);
		if (parent == procs.end() || parent->second.second != want)
			return pid;
		if (!fallback)
			fallback = pid;
	}
	return fallback;
}

class ActivationHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
	ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
	~ActivationHandler() { CloseHandle(done); }
	STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *) override
	{
		SetEvent(done);
		return S_OK;
	}
	HANDLE done;
};

// Client audio du son d'un processus (et de ses sous-processus), ou de tout sauf lui
ComPtr<IAudioClient> activate_process_loopback(DWORD pid, bool include)
{
	AUDIOCLIENT_ACTIVATION_PARAMS params{};
	params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	params.ProcessLoopbackParams.TargetProcessId = pid;
	params.ProcessLoopbackParams.ProcessLoopbackMode =
		include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
	PROPVARIANT pv{};
	pv.vt = VT_BLOB;
	pv.blob.cbSize = sizeof(params);
	pv.blob.pBlobData = (BYTE *)&params;

	auto handler = Microsoft::WRL::Make<ActivationHandler>();
	ComPtr<IActivateAudioInterfaceAsyncOperation> op;
	if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
					       handler.Get(), &op)) ||
	    WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0)
		return nullptr;

	HRESULT hr = E_FAIL;
	ComPtr<IUnknown> unk;
	ComPtr<IAudioClient> client;
	if (FAILED(op->GetActivateResult(&hr, &unk)) || FAILED(hr) || FAILED(unk.As(&client)))
		return nullptr;
	return client;
}

ComPtr<IMMDevice> find_capture_device(const std::wstring &match)
{
	ComPtr<IMMDeviceEnumerator> en;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
		return nullptr;
	ComPtr<IMMDevice> dev;
	if (match.empty()) {
		en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
		return dev;
	}
	ComPtr<IMMDeviceCollection> list;
	en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list);
	UINT n = 0;
	if (list)
		list->GetCount(&n);
	for (UINT i = 0; i < n; i++) {
		ComPtr<IMMDevice> d;
		ComPtr<IPropertyStore> props;
		PROPVARIANT name;
		PropVariantInit(&name);
		if (SUCCEEDED(list->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) &&
		    SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
		    lower(name.pwszVal).find(lower(match)) != std::wstring::npos) {
			PropVariantClear(&name);
			return d;
		}
		PropVariantClear(&name);
	}
	return nullptr;
}

// Capture WASAPI partagée, pilotée par événement, convertie par Windows en 48 kHz stéréo
// flottant quel que soit le format du périphérique.
class WasapiInput final : public AudioInput {
public:
	enum class Kind { App, System, Mic };

	WasapiInput(Kind kind, std::wstring target) : kind_(kind), target_(std::move(target))
	{
		switch (kind_) {
		case Kind::App:
			name_ = to_utf8(target_);
			if (name_.size() > 4 && _stricmp(name_.c_str() + name_.size() - 4, ".exe") == 0)
				name_.resize(name_.size() - 4);
			break;
		case Kind::System:
			name_ = "Son du système";
			break;
		case Kind::Mic:
			name_ = target_.empty() ? "Micro" : "Micro " + to_utf8(target_);
			break;
		}
	}
	~WasapiInput() override { stop(); }

	bool start(OnSamples on_samples) override
	{
		on_samples_ = std::move(on_samples);
		running_ = true;
		thread_ = std::thread(&WasapiInput::run, this);
		return true;
	}

	void stop() override
	{
		if (running_.exchange(false) && thread_.joinable())
			thread_.join();
	}

	std::string name() const override { return name_; }

private:
	ComPtr<IAudioClient> activate(HANDLE &process)
	{
		process = nullptr;
		if (kind_ == Kind::System)
			return activate_process_loopback(GetCurrentProcessId(), false);
		if (kind_ == Kind::App) {
			const DWORD pid = find_root_process(target_);
			if (!pid)
				return nullptr;
			process = OpenProcess(SYNCHRONIZE, FALSE, pid);
			FLOG_I("Son de « %s » : processus %lu", name_.c_str(), (unsigned long)pid);
			return activate_process_loopback(pid, true);
		}
		ComPtr<IMMDevice> dev = find_capture_device(target_);
		ComPtr<IAudioClient> client;
		if (dev)
			dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)client.GetAddressOf());
		return client;
	}

	// Une session de capture, jusqu'à la fin de l'appli suivie ou une erreur
	void capture(IAudioClient *client, HANDLE process)
	{
		WAVEFORMATEX fmt{};
		fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		fmt.nChannels = 2;
		fmt.nSamplesPerSec = kRate;
		fmt.wBitsPerSample = 32;
		fmt.nBlockAlign = 8;
		fmt.nAvgBytesPerSec = kRate * 8;

		DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
			      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
		if (kind_ != Kind::Mic)
			flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
		HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &fmt, nullptr);
		if (FAILED(hr)) {
			FLOG_E("Initialisation de « %s » impossible (0x%08lx)", name_.c_str(), (unsigned long)hr);
			return;
		}

		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		ComPtr<IAudioCaptureClient> cap;
		client->SetEventHandle(event);
		if (FAILED(client->GetService(IID_PPV_ARGS(&cap))) || FAILED(client->Start())) {
			CloseHandle(event);
			return;
		}

		std::vector<float> silence;
		while (running_) {
			WaitForSingleObject(event, 100);
			if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
				FLOG_I("« %s » s'est fermée, on attend qu'elle revienne", name_.c_str());
				break;
			}
			UINT32 packet = 0;
			while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
				BYTE *data = nullptr;
				UINT32 frames = 0;
				DWORD bflags = 0;
				UINT64 qpc = 0;
				if (FAILED(cap->GetBuffer(&data, &frames, &bflags, nullptr, &qpc)))
					break;

				// Heure du premier échantillon : celle que donne le moteur audio (unités de
				// 100 ns de l'horloge QPC), sinon estimée depuis maintenant
				uint64_t first_ns = qpc * 100;
				if (!qpc || (bflags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))
					first_ns = now_ns() - (uint64_t)frames * 1'000'000'000ULL / kRate;

				const float *samples = (const float *)data;
				if (bflags & AUDCLNT_BUFFERFLAGS_SILENT) {
					silence.assign((size_t)frames * 2, 0.0f);
					samples = silence.data();
				}
				on_samples_(samples, frames, first_ns);
				cap->ReleaseBuffer(frames);
			}
		}
		client->Stop();
		CloseHandle(event);
	}

	void run()
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD task = 0;
		HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
		bool warned = false;

		while (running_) {
			HANDLE process = nullptr;
			ComPtr<IAudioClient> client = activate(process);
			if (client) {
				warned = false;
				capture(client.Get(), process);
			} else if (!warned) {
				FLOG_W("« %s » introuvable pour l'instant, recherche en continu", name_.c_str());
				warned = true;
			}
			if (process)
				CloseHandle(process);
			for (int i = 0; i < 20 && running_; i++)
				Sleep(100);
		}

		if (mm)
			AvRevertMmThreadCharacteristics(mm);
		CoUninitialize();
	}

	const Kind kind_;
	const std::wstring target_;
	std::string name_;
	OnSamples on_samples_;
	std::atomic<bool> running_{false};
	std::thread thread_;
};

} // namespace

std::unique_ptr<AudioInput> make_app_audio_input(const std::string &exe)
{
	return std::make_unique<WasapiInput>(WasapiInput::Kind::App, to_wide(exe));
}

std::unique_ptr<AudioInput> make_system_audio_input()
{
	return std::make_unique<WasapiInput>(WasapiInput::Kind::System, L"");
}

std::unique_ptr<AudioInput> make_mic_input(const std::string &match)
{
	return std::make_unique<WasapiInput>(WasapiInput::Kind::Mic, to_wide(match));
}

std::vector<AudioAppInfo> list_audio_apps()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	std::map<std::wstring, AudioAppInfo> apps;

	ComPtr<IMMDeviceEnumerator> en;
	ComPtr<IMMDeviceCollection> devices;
	UINT n = 0;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
	    SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
		devices->GetCount(&n);

	for (UINT i = 0; i < n; i++) {
		ComPtr<IMMDevice> dev;
		ComPtr<IAudioSessionManager2> mgr;
		ComPtr<IAudioSessionEnumerator> sessions;
		if (FAILED(devices->Item(i, &dev)) ||
		    FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)mgr.GetAddressOf())) ||
		    FAILED(mgr->GetSessionEnumerator(&sessions)))
			continue;
		int count = 0;
		sessions->GetCount(&count);
		for (int k = 0; k < count; k++) {
			ComPtr<IAudioSessionControl> ctl;
			ComPtr<IAudioSessionControl2> ctl2;
			DWORD pid = 0;
			AudioSessionState state = AudioSessionStateInactive;
			if (FAILED(sessions->GetSession(k, &ctl)) || FAILED(ctl.As(&ctl2)) ||
			    FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == GetCurrentProcessId())
				continue;
			ctl->GetState(&state);
			const std::wstring exe = exe_of_pid(pid);
			if (exe.empty())
				continue;
			AudioAppInfo &info = apps[lower(exe)];
			info.exe = to_utf8(exe);
			info.pid = pid;
			info.playing = info.playing || state == AudioSessionStateActive;
		}
	}

	std::vector<AudioAppInfo> out;
	for (auto &[k, v] : apps)
		out.push_back(v);
	return out;
}

std::vector<std::string> list_microphones()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	std::vector<std::string> out;
	ComPtr<IMMDeviceEnumerator> en;
	ComPtr<IMMDeviceCollection> list;
	UINT n = 0;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
	    SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list)))
		list->GetCount(&n);
	for (UINT i = 0; i < n; i++) {
		ComPtr<IMMDevice> d;
		ComPtr<IPropertyStore> props;
		PROPVARIANT name;
		PropVariantInit(&name);
		if (SUCCEEDED(list->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) &&
		    SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
			out.push_back(to_utf8(name.pwszVal));
		PropVariantClear(&name);
	}
	return out;
}

} // namespace flash
