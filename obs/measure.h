#pragma once

#include <cstdint>

// Journal de mesure, actif seulement si la variable d'environnement FLASH_MESURE donne un
// fichier CSV. Toutes les heures sont dans l'horloge d'OBS (QPC en ns).
namespace measure {

bool enabled();

// Image décodée : numéro et heure de rendu lus dans la mire, horodatage de l'émetteur
// (capture), puis les étapes côté OBS.
void frame(uint32_t frame, uint64_t capture_ns, uint64_t arrival_ns, uint64_t decoded_ns, uint64_t release_ns,
	   uint64_t key, bool flash, uint64_t pts_ns);
// Image remise à une source, puis rendu d'OBS : l'image affichée à un rendu est la
// dernière remise avant lui (source non tamponnée). On note le premier rendu de chaque image
// sans rien prendre à OBS.
void released(const void *source, uint64_t key);
void tick(const void *source, uint64_t tick_ns);
// Début d'un bip dans le son remis à OBS, dans la chronologie qu'OBS lui donne.
void beep(uint64_t onset_ns);
// Point d'étape de la connexion : décalage d'horloge, phase, et retard maximal à
// l'arrivée (arrivée - horodatage) de la vidéo et du son sur la dernière seconde.
void clock(int64_t offset_ns, int64_t phase_ns, int64_t video_lag_ns, int64_t audio_lag_ns);

void shutdown();

} // namespace measure
