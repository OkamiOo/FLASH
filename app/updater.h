#pragma once

#include <string>

// Mises à jour automatiques signées, plugin OBS et retrait complet, pour les deux éditions
// (portable et installée). Le dossier de FLASH contient l'appli et une copie du plugin
// (plugin-obs\) ; le plugin actif vit dans %ProgramData%\obs-studio\plugins\flash-obs.
namespace updater {

enum class Result { Nothing, Installed, Failed };

// Au lancement : efface les restes de la mise à jour précédente. Rend la version qui vient
// d'être installée (vide sinon), pour dire « FLASH est à jour ».
std::string after_launch();
// Cherche une mise à jour et l'installe (fil d'arrière-plan). message : version installée ou erreur.
Result check(std::string &message);
bool has_address(); // une adresse de publication est connue

// Plugin OBS
enum class Plugin { NoSource, Absent, UpToDate, Different };
Plugin plugin_state();
bool obs_running();
// Copie (ou retire) le plugin ; demande l'élévation seulement si Windows refuse la copie directe.
// Rend un message d'erreur, vide si tout va bien.
std::string install_plugin();
std::string remove_plugin();
bool plugin_offered();      // la page « Installer le plugin dans OBS ? » a déjà été montrée
void set_plugin_offered();

// Retrait complet, après le plugin : lancement avec Windows, réglages et cache de l'interface,
// une fois le moteur et l'interface arrêtés (sinon le moteur réécrit ses réglages en partant)
void remove_settings();
bool installed_edition(); // installée par l'installeur (sinon portable)
void run_uninstaller();

// Ligne de commande : --verifier-maj, --plugin installer|retirer (côté élevé), --plugin-retrait
// (désinstalleur). -1 : pas pour nous.
int run_command(int argc, char **argv);

} // namespace updater
