#pragma once

#include "flash/sender.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flash {

// Mire de mesure et bip, calés sur la même heure de départ t0.
std::unique_ptr<VideoInput> make_mire_input(uint64_t t0);
std::unique_ptr<AudioInput> make_beep_input(uint64_t t0);

// Capture par Windows.Graphics.Capture. L'image est ramenée dans max_w × max_h en gardant
// ses proportions. Écran : numéro à partir de 1. Fenêtre : morceau de titre ou nom
// d'exécutable ; si elle disparaît, elle est recherchée à nouveau.
std::unique_ptr<VideoInput> make_screen_input(int index, uint32_t max_w, uint32_t max_h, bool cursor);
std::unique_ptr<VideoInput> make_window_input(const std::string &match, uint32_t max_w, uint32_t max_h, bool cursor);

// Son d'une application (nom d'exécutable, sous-processus compris), de tout le système
// sauf FLASH, ou d'un micro (morceau de nom, vide = micro par défaut).
std::unique_ptr<AudioInput> make_app_audio_input(const std::string &exe);
std::unique_ptr<AudioInput> make_system_audio_input();
std::unique_ptr<AudioInput> make_mic_input(const std::string &match);

struct ScreenInfo {
	int index;
	std::string name;
	int x;
	int y;
	int width;
	int height;
	int hz;
	std::string adapter;
	bool primary;
};

struct WindowInfo {
	std::string title;
	std::string exe;
};

struct AudioAppInfo {
	std::string exe;
	uint32_t pid;
	bool playing; // son en cours de lecture
};

std::vector<ScreenInfo> list_screens();
std::vector<WindowInfo> list_windows();
std::vector<AudioAppInfo> list_audio_apps();
std::vector<std::string> list_microphones();

// Taille d'encodage : la source ramenée dans max_w × max_h, proportions gardées, côtés pairs.
void fit_size(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h, uint32_t &w, uint32_t &h);

std::wstring to_wide(const std::string &s);
std::string to_utf8(const std::wstring &s);

} // namespace flash
