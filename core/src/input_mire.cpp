#include "flash/clock.h"
#include "flash/inputs.h"
#include "flash/log.h"
#include "flash/mire.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace flash {

namespace {

// Images de la mire, dessinées en NV12 directement sur le GPU de l'encodeur, à 60 i/s
// pile à partir de t0. L'heure de capture inscrite est l'heure prévue de l'image.
class MireInput final : public VideoInput {
public:
	explicit MireInput(uint64_t t0) : t0_(t0) {}
	~MireInput() override { stop(); }

	bool start(ID3D11Device *device, OnFrame on_frame) override
	{
		if (!init(device))
			return false;
		on_frame_ = std::move(on_frame);
		running_ = true;
		thread_ = std::thread(&MireInput::run, this);
		return true;
	}

	void stop() override
	{
		if (running_.exchange(false) && thread_.joinable())
			thread_.join();
	}

	uint32_t width() const override { return mire::kWidth; }
	uint32_t height() const override { return mire::kHeight; }
	std::string name() const override { return "Mire"; }

private:
	bool init(ID3D11Device *device)
	{
		device->GetImmediateContext(&context_);
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = mire::kWidth;
		desc.Height = mire::kHeight;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		for (auto &t : staging_)
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &t)))
				return false;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		for (auto &t : frames_)
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &t)))
				return false;
		background_.resize(mire::kWidth);
		for (uint32_t x = 0; x < mire::kWidth; x++)
			background_[x] = (uint8_t)(40 + x * 60 / mire::kWidth);
		return true;
	}

	ID3D11Texture2D *render(uint32_t frame, uint64_t capture_ns)
	{
		ID3D11Texture2D *staging = staging_[frame & 1].Get();
		D3D11_MAPPED_SUBRESOURCE map;
		if (FAILED(context_->Map(staging, 0, D3D11_MAP_WRITE, 0, &map)))
			return nullptr;

		uint8_t *y = (uint8_t *)map.pData;
		const size_t pitch = map.RowPitch;
		for (uint32_t line = 0; line < mire::kHeight; line++)
			std::memcpy(y + line * pitch, background_.data(), mire::kWidth);

		// Une barre qui défile, sous la zone du flash, pour que l'image bouge
		const uint32_t bar_x = (frame * 12) % (mire::kWidth - 64);
		for (uint32_t line = 780; line < mire::kHeight - 40; line++)
			std::memset(y + line * pitch + bar_x, 200, 64);

		mire::draw_code(y, pitch, capture_ns, frame);
		if (frame % mire::kFps == 0)
			mire::draw_flash(y, pitch);

		uint8_t *uv = y + pitch * mire::kHeight;
		for (uint32_t line = 0; line < mire::kHeight / 2; line++)
			std::memset(uv + line * pitch, 128, mire::kWidth);
		context_->Unmap(staging, 0);

		ID3D11Texture2D *out = frames_[frame % frames_.size()].Get();
		context_->CopyResource(out, staging);
		return out;
	}

	void run()
	{
		for (uint32_t n = 0; running_; n++) {
			const uint64_t t = t0_ + (uint64_t)n * 1'000'000'000ULL / mire::kFps;
			sleep_until_ns(t);
			if (ID3D11Texture2D *tex = render(n, t))
				on_frame_(tex, t);
		}
	}

	const uint64_t t0_;
	OnFrame on_frame_;
	std::atomic<bool> running_{false};
	std::thread thread_;
	ComPtr<ID3D11DeviceContext> context_;
	std::array<ComPtr<ID3D11Texture2D>, 2> staging_;
	std::array<ComPtr<ID3D11Texture2D>, 3> frames_;
	std::vector<uint8_t> background_;
};

// Bip de 1 kHz pendant 50 ms à chaque seconde pile après t0, livré par blocs de 10 ms
// au moment où le bloc est complet, comme le ferait une carte son.
class BeepInput final : public AudioInput {
public:
	explicit BeepInput(uint64_t t0) : t0_(t0) {}
	~BeepInput() override { stop(); }

	bool start(OnSamples on_samples) override
	{
		on_samples_ = std::move(on_samples);
		running_ = true;
		thread_ = std::thread(&BeepInput::run, this);
		return true;
	}

	void stop() override
	{
		if (running_.exchange(false) && thread_.joinable())
			thread_.join();
	}

	std::string name() const override { return "Bip"; }

private:
	void run()
	{
		constexpr uint32_t kBlock = 480;
		float buf[kBlock * 2];
		const double w = 2.0 * 3.14159265358979323846 * mire::kBeepHz / mire::kSampleRate;
		for (uint64_t m = 0; running_; m++) {
			const uint64_t first = m * kBlock;
			sleep_until_ns(t0_ + (first + kBlock) * 1'000'000'000ULL / mire::kSampleRate);
			for (uint32_t i = 0; i < kBlock; i++) {
				const uint64_t phase = (first + i) % mire::kSampleRate;
				const float v = phase < mire::kBeepSamples ? (float)(0.5 * std::sin(w * (double)phase)) : 0.0f;
				buf[2 * i] = buf[2 * i + 1] = v;
			}
			on_samples_(buf, kBlock, t0_ + first * 1'000'000'000ULL / mire::kSampleRate);
		}
	}

	const uint64_t t0_;
	OnSamples on_samples_;
	std::atomic<bool> running_{false};
	std::thread thread_;
};

} // namespace

std::unique_ptr<VideoInput> make_mire_input(uint64_t t0)
{
	return std::make_unique<MireInput>(t0);
}

std::unique_ptr<AudioInput> make_beep_input(uint64_t t0)
{
	return std::make_unique<BeepInput>(t0);
}

} // namespace flash
