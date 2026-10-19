#include "flash/zip.h"

#include <zlib.h>

#include <cstring>

namespace flash::zip {

namespace {

uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

constexpr uint32_t kEnd = 0x06054b50, kCentral = 0x02014b50, kLocal = 0x04034b50;

} // namespace

bool list(const std::vector<uint8_t> &a, std::vector<Entry> &entries)
{
	entries.clear();
	if (a.size() < 22)
		return false;
	// Fin du répertoire central : dans les 64 Ko de la fin (commentaire compris)
	size_t end = std::string::npos;
	const size_t stop = a.size() > 22 + 65535 ? a.size() - 22 - 65535 : 0;
	for (size_t i = a.size() - 22 + 1; i-- > stop;) {
		if (u32(&a[i]) == kEnd) {
			end = i;
			break;
		}
	}
	if (end == std::string::npos)
		return false;
	const uint16_t count = u16(&a[end + 10]);
	size_t pos = u32(&a[end + 16]);
	for (uint16_t n = 0; n < count; n++) {
		if (pos + 46 > a.size() || u32(&a[pos]) != kCentral)
			return false;
		const uint8_t *c = &a[pos];
		Entry e;
		e.method = u16(c + 10);
		e.crc = u32(c + 16);
		e.compressed = u32(c + 20);
		e.size = u32(c + 24);
		const uint16_t name_len = u16(c + 28), extra_len = u16(c + 30), comment_len = u16(c + 32);
		e.local_offset = u32(c + 42);
		if (pos + 46 + name_len > a.size())
			return false;
		e.name.assign((const char *)c + 46, name_len);
		pos += 46 + (size_t)name_len + extra_len + comment_len;
		if (!e.name.empty() && e.name.back() != '/') // les dossiers ne sont pas des fichiers
			entries.push_back(std::move(e));
	}
	return true;
}

bool extract(const std::vector<uint8_t> &a, const Entry &e, std::vector<uint8_t> &out, size_t max_size)
{
	out.clear();
	if (e.size > max_size || e.local_offset + 30 > a.size() || u32(&a[e.local_offset]) != kLocal)
		return false;
	const uint8_t *l = &a[e.local_offset];
	const size_t data = e.local_offset + 30 + u16(l + 26) + u16(l + 28);
	if (data + e.compressed > a.size())
		return false;
	out.resize((size_t)e.size);
	if (e.size == 0) // fichier vide : rien à décompresser (zlib refuse une sortie vide)
		return e.crc == 0;
	if (e.method == 0) {
		if (e.compressed != e.size)
			return false;
		if (e.size)
			std::memcpy(out.data(), &a[data], (size_t)e.size);
	} else if (e.method == 8) {
		// Deflate brut, sans en-tête zlib
		z_stream s{};
		if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
			return false;
		s.next_in = (Bytef *)&a[data];
		s.avail_in = (uInt)e.compressed;
		s.next_out = out.data();
		s.avail_out = (uInt)out.size();
		const int r = inflate(&s, Z_FINISH);
		const bool ok = r == Z_STREAM_END && s.total_out == e.size;
		inflateEnd(&s);
		if (!ok)
			return false;
	} else {
		return false;
	}
	return crc32(0, out.data(), (uInt)out.size()) == e.crc;
}

} // namespace flash::zip
