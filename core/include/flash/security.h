#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flash {

void random_bytes(void *buf, size_t size);

std::string to_hex(const uint8_t *data, size_t size);
bool from_hex(const std::string &hex, std::vector<uint8_t> &out);

// Identifiant d'appareil : 16 octets aléatoires en hexadécimal, tiré une fois pour toutes.
std::string new_device_id();

// Code d'invitation de 8 caractères (alphabet sans I, L, O ni U), affiché « 7KQ4-M9XP ».
std::string new_invitation_code();
// Forme comparable d'un code tapé : majuscules, sans tiret ni espace, O lu comme 0, I et L comme 1.
std::string normalize_code(const std::string &code);

// Secret chiffré pour l'utilisateur Windows courant (DPAPI), en base64 pour un fichier de réglages.
std::string protect_secret(const std::string &secret);
std::string unprotect_secret(const std::string &blob);

} // namespace flash
