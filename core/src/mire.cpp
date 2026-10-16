#include "flash/mire.h"

#include <cstring>

namespace flash::mire {

namespace {

constexpr uint8_t kHigh = 235;
constexpr uint8_t kLow = 16;

// Quatre lignes de 64 bits : heure, heure inversée, numéro + numéro inversé
void draw_row(uint8_t *y, size_t pitch, int row, uint64_t bits)
{
	for (int b = 0; b < 64; b++) {
		const uint8_t v = (bits >> (63 - b)) & 1 ? kHigh : kLow;
		uint8_t *p = y + (size_t)(kY0 + row * kBlock) * pitch + kX0 + b * kBlock;
		for (int line = 0; line < kBlock; line++)
			std::memset(p + line * pitch, v, kBlock);
	}
}

uint64_t read_row(const uint8_t *y, size_t pitch, int row)
{
	uint64_t bits = 0;
	for (int b = 0; b < 64; b++) {
		// Moyenne de 4×4 pixels au centre du bloc : insensible aux bavures du codage
		const int cx = kX0 + b * kBlock + kBlock / 2 - 2;
		const int cy = kY0 + row * kBlock + kBlock / 2 - 2;
		unsigned sum = 0;
		for (int dy = 0; dy < 4; dy++)
			for (int dx = 0; dx < 4; dx++)
				sum += y[(size_t)(cy + dy) * pitch + cx + dx];
		bits = (bits << 1) | (sum / 16 > 128 ? 1 : 0);
	}
	return bits;
}

} // namespace

void draw_code(uint8_t *y, size_t pitch, uint64_t capture_ns, uint32_t frame)
{
	draw_row(y, pitch, 0, capture_ns);
	draw_row(y, pitch, 1, ~capture_ns);
	draw_row(y, pitch, 2, ((uint64_t)frame << 32) | (uint32_t)~frame);
}

void draw_flash(uint8_t *y, size_t pitch)
{
	for (int line = 0; line < kFlashSize; line++)
		std::memset(y + (size_t)(kFlashY + line) * pitch + kFlashX, kHigh, kFlashSize);
}

bool read_code(const uint8_t *y, size_t pitch, uint64_t &capture_ns, uint32_t &frame)
{
	const uint64_t t = read_row(y, pitch, 0);
	const uint64_t t_inv = read_row(y, pitch, 1);
	const uint64_t f = read_row(y, pitch, 2);
	if (t != ~t_inv || (uint32_t)(f >> 32) != (uint32_t)~(uint32_t)f)
		return false;
	capture_ns = t;
	frame = (uint32_t)(f >> 32);
	return true;
}

bool flash_visible(const uint8_t *y, size_t pitch)
{
	const int cx = kFlashX + kFlashSize / 2 - 16;
	const int cy = kFlashY + kFlashSize / 2 - 16;
	unsigned sum = 0;
	for (int dy = 0; dy < 32; dy++)
		for (int dx = 0; dx < 32; dx++)
			sum += y[(size_t)(cy + dy) * pitch + cx + dx];
	return sum / 1024 > 215; // la barre mobile (200) ne doit pas passer pour un flash
}

} // namespace flash::mire
