#pragma once

#include <cstddef>
#include <cstdint>

// Mire de mesure : l'émetteur inscrit dans l'image son heure de capture et son numéro sous
// forme de code-barres, et allume un carré blanc à chaque seconde pile, au moment exact où
// démarre un bip de 1 kHz dans le son. Le récepteur relit tout ça dans l'image décodée.

namespace flash::mire {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kFps = 60;

constexpr int kBlock = 16;         // côté d'un bit, en pixels
constexpr int kX0 = 32, kY0 = 32;  // coin du code-barres
constexpr int kFlashX = 1360, kFlashY = 340, kFlashSize = 400;

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBeepHz = 1000;
constexpr uint32_t kBeepSamples = kSampleRate / 20; // 50 ms

// Dessine le code-barres (heure de capture, numéro d'image, contrôles) dans un plan Y.
void draw_code(uint8_t *y, size_t pitch, uint64_t capture_ns, uint32_t frame);
void draw_flash(uint8_t *y, size_t pitch);

// Relit le code-barres. Faux si l'image est abîmée ou n'est pas une mire.
bool read_code(const uint8_t *y, size_t pitch, uint64_t &capture_ns, uint32_t &frame);
bool flash_visible(const uint8_t *y, size_t pitch);

} // namespace flash::mire
