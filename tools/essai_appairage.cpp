// Outil d'essai (non livré) : appaire le plugin d'un OBS de test avec un émetteur, comme le
// ferait le bouton « Appairer » des propriétés d'une source FLASH.
// Usage : essai-appairage <adresse> <code> <flash.json du plugin>

#include <flash/discovery.h>
#include <flash/inputs.h>
#include <flash/pairing.h>
#include <flash/security.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

using json = nlohmann::json;

int main(int argc, char **argv)
{
	if (argc != 4) {
		std::printf("Usage : essai-appairage <adresse> <code> <flash.json du plugin>\n");
		return 2;
	}
	const std::wstring path = flash::to_wide(argv[3]);

	json j;
	{
		std::ifstream in(path);
		if (in)
			j = json::parse(in, nullptr, false);
	}
	if (!j.is_object())
		j = json::object();
	if (j.value("id", "").empty())
		j["id"] = flash::new_device_id();
	if (!j.contains("emetteurs") || !j["emetteurs"].is_array())
		j["emetteurs"] = json::array();

	const flash::PairingResult r =
		flash::pair_with_sender(argv[1], flash::kPairingPort, argv[2], {j["id"].get<std::string>(), "OBS de test"});
	if (!r.ok) {
		std::printf("Échec : %s\n", r.error.c_str());
		return 1;
	}

	json list = json::array();
	for (const auto &e : j["emetteurs"])
		if (e.value("id", "") != r.sender.id)
			list.push_back(e);
	list.push_back({{"id", r.sender.id}, {"nom", r.sender.name}, {"cle", flash::protect_secret(r.key_hex)}});
	j["emetteurs"] = list;
	std::ofstream(path, std::ios::trunc) << j.dump(2);
	std::printf("Plugin appairé avec « %s »\n", r.sender.name.c_str());
	return 0;
}
