#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Lecture d'une archive zip en mémoire (répertoire central, entrées stockées ou compressées en
// deflate), pour les mises à jour : le zip portable porte les fichiers et le manifeste signé.
namespace flash::zip {

struct Entry {
	std::string name; // chemin dans l'archive, « / » entre les dossiers
	uint16_t method = 0;
	uint32_t crc = 0;
	uint64_t compressed = 0, size = 0;
	uint64_t local_offset = 0;
};

// Faux si l'archive est illisible
bool list(const std::vector<uint8_t> &archive, std::vector<Entry> &entries);
// Décompresse une entrée et vérifie son CRC-32 ; faux sinon
bool extract(const std::vector<uint8_t> &archive, const Entry &entry, std::vector<uint8_t> &out, size_t max_size = 64u << 20);

} // namespace flash::zip
