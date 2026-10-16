#pragma once

#include <flash/discovery.h>

#include <optional>
#include <string>
#include <vector>

// Ce que le plugin sait des émetteurs : ceux du réseau (découverte, gardée quelques secondes)
// et ceux avec qui il est appairé (clé chiffrée pour l'utilisateur Windows, dans le dossier de
// configuration du plugin).
namespace directory {

// Identité de ce récepteur auprès des émetteurs
std::string self_id();
std::string self_name();

// Émetteurs du réseau ; refresh : nouvelle recherche même si la précédente est récente
std::vector<flash::SenderAnnounce> senders(bool refresh);
std::optional<flash::SenderAnnounce> find(const std::string &id, bool refresh);

struct Paired {
	std::string id;
	std::string name;
	std::string key;
};

std::vector<Paired> paired();
std::optional<Paired> paired_with(const std::string &id);
void remember(const Paired &p);
void forget(const std::string &id);

} // namespace directory
