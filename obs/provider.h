#pragma once

// Envoi depuis OBS : le plugin sert à l'appli FLASH de ce PC la liste des scènes et sources,
// leurs aperçus, et l'image et le son de celles qu'elle envoie (voir CONCEPTION.md, § 15).
// Personne de connecté : il ne fait rien.
namespace provider {

void start();
void stop();

} // namespace provider
