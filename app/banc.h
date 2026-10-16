#pragma once

#include <string>

// Banc d'essai : fenêtre « Banc FLASH » (image + bips) à capturer comme un jeu.
// output : morceau du nom de la sortie audio (vide = sortie par défaut).
// screen : écran où ouvrir la fenêtre (1 = principal).
int run_banc(const std::string &output, int screen, int duration_s);
