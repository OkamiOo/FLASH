#include "mire_source.h"

#include <flash/log.h>
#include <flash/mire.h>

#include <cstring>

using namespace flash;

bool MireSource::init(ID3D11Device *device)
{
	device_ = device;
	device->GetImmediateContext(&context_);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = mire::kWidth;
	desc.Height = mire::kHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_NV12;
	desc.SampleDesc.Count = 1;

	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	for (auto &t : staging_) {
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &t)))
			return false;
	}

	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	for (auto &t : frames_) {
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &t))) {
			FLOG_E("Création des textures NV12 impossible");
			return false;
		}
	}

	background_.resize(mire::kWidth);
	for (uint32_t x = 0; x < mire::kWidth; x++)
		background_[x] = (uint8_t)(40 + x * 60 / mire::kWidth);
	return true;
}

ID3D11Texture2D *MireSource::render(uint32_t frame, uint64_t capture_ns)
{
	ID3D11Texture2D *staging = staging_[frame & 1].Get();
	D3D11_MAPPED_SUBRESOURCE map;
	if (FAILED(context_->Map(staging, 0, D3D11_MAP_WRITE, 0, &map)))
		return nullptr;

	uint8_t *y = (uint8_t *)map.pData;
	const size_t pitch = map.RowPitch;
	for (uint32_t line = 0; line < mire::kHeight; line++)
		std::memcpy(y + line * pitch, background_.data(), mire::kWidth);

	// Une barre qui défile, pour que l'image bouge comme une vraie source
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

	ID3D11Texture2D *out = frames_[index_].Get();
	index_ = (index_ + 1) % (uint32_t)frames_.size();
	context_->CopyResource(out, staging);
	return out;
}
