#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Mises à jour signées : empreintes SHA-256, signature Ed25519 (RFC 8032) du manifeste,
// téléchargement HTTPS. La signature fait foi, pas l'adresse de téléchargement.

namespace flash::update {

using Sha256 = std::array<uint8_t, 32>;

Sha256 sha256(const void *data, size_t size);
std::string hex(const uint8_t *data, size_t size);
bool from_hex(const std::string &text, std::vector<uint8_t> &out);

// Ed25519 : clé publique tirée de la graine (32 octets secrets), signature, vérification
void ed25519_public_key(uint8_t public_key[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const void *message, size_t size);
bool ed25519_check(const uint8_t signature[64], const uint8_t public_key[32], const void *message, size_t size);

struct File {
	std::string path; // relatif au dossier de FLASH, « / » entre les dossiers
	uint64_t size = 0;
	Sha256 sha256{};
};

struct Manifest {
	std::string version;
	std::string notes;
	std::vector<File> files;
};

// Format « FLASH-MAJ 1 » : une ligne « version x.y.z », une ligne « notes ... », puis une ligne
// « fichier <taille> <sha256> <chemin> » par fichier. Faux (et error rempli) si la signature
// ne correspond pas à la clé publique ou si le texte est mal formé.
bool parse_manifest(const std::string &text, const std::string &signature_hex, const uint8_t public_key[32],
		    Manifest &out, std::string &error);
std::string write_manifest(const Manifest &m);

// Chemin relatif sans danger : pas de lecteur, pas de « .. », pas de dossier caché
bool safe_path(const std::string &path);

// « 0.10.0 » plus récente que « 0.9.3 »
bool newer(const std::string &a, const std::string &b);

// Téléchargement entier en mémoire. HTTPS seulement, sauf vers ce PC (essais).
bool download(const std::string &url, std::vector<uint8_t> &out, std::string &error, size_t max_size = 64u << 20);

} // namespace flash::update
