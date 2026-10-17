#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Lien local entre le plugin OBS (qui sert) et l'appli FLASH (qui envoie) sur le même PC :
// un tube nommé, réservé à l'utilisateur. Chaque message : sa longueur sur 4 octets
// (petit-boutiste, type compris), son type sur 1 octet, puis son contenu.

namespace flash::obslink {

inline constexpr wchar_t kPipe[] = L"\\\\.\\pipe\\flash-obs";

enum Type : uint8_t {
	Catalog = 1,   // plugin -> appli, JSON : {"scenes":[{nom}], "sources":[{nom, video, audio, largeur, hauteur}], "base":[l, h]}
	Open = 2,      // appli -> plugin, JSON : {id, type ("scene", "source", "programme"), nom, largeur, hauteur}
	Opened = 3,    // plugin -> appli, JSON : {id, largeur, hauteur, poignees:[3]} textures BGRA partagées, verrou à clé
	Frame = 4,     // plugin -> appli : u32 id, u32 case, u64 heure (ns, QPC). Clé 1 : à l'appli ; elle rend la clé 0
	Close = 5,     // appli -> plugin, JSON : {id}
	OpenAudio = 6, // appli -> plugin, JSON : {id, nom} (nom vide : le mix d'OBS)
	Audio = 7,     // plugin -> appli : u32 id, u64 heure du premier échantillon, u32 échantillons, stéréo 48 kHz flottant
	Previews = 8,  // appli -> plugin, JSON : {actif}
	Preview = 9,   // plugin -> appli : u16 longueur du nom, nom, u16 largeur, u16 hauteur, BGRA
	Closed = 10,   // plugin -> appli, JSON : {id} la source a disparu
};

struct Message {
	uint8_t type = 0;
	std::vector<uint8_t> body;
};

std::vector<uint8_t> pack(uint8_t type, const void *data, size_t size);
std::vector<uint8_t> pack(uint8_t type, const std::string &json);

// Un tube connecté, ouvert en mode asynchrone : lectures et écritures depuis deux fils, toutes
// deux interrompues quand l'événement stop est signalé.
class Link {
public:
	Link(void *pipe, void *stop);
	~Link();
	bool read(Message &m);
	bool write(const std::vector<uint8_t> &bytes);

private:
	bool io(bool reading, void *buf, uint32_t size);

	void *pipe_;
	void *stop_;
	void *read_event_;
	void *write_event_;
};

} // namespace flash::obslink
