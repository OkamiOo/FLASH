#include "flash/ts.h"

namespace flash {

// CRC-32 des sections MPEG (polynôme 0x04C11DB7, sans réflexion)
uint32_t crc32_mpeg(const uint8_t *data, size_t size)
{
	static const auto table = [] {
		std::array<uint32_t, 256> t{};
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i << 24;
			for (int k = 0; k < 8; k++)
				c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : c << 1;
			t[i] = c;
		}
		return t;
	}();

	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; i++)
		crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFF];
	return crc;
}

} // namespace flash
