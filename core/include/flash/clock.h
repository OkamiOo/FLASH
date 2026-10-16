#pragma once

#include <cstdint>

namespace flash {

// Horloge commune à tout FLASH : le compteur QPC en nanosecondes, calculé exactement comme
// os_gettime_ns() d'OBS. Sur un même PC, émetteur et OBS lisent donc la même heure.
uint64_t now_ns();

// Convertit une valeur brute du compteur QPC en nanosecondes.
uint64_t qpc_to_ns(int64_t qpc);

// Attend jusqu'à l'heure donnée avec un minuteur haute résolution (précision ~0,5 ms).
void sleep_until_ns(uint64_t t);

// Demande au système une résolution de minuterie de 1 ms pour ce processus.
void enable_precise_timers();

// Conversions vers et depuis l'horloge 90 kHz du MPEG-TS (33 bits).
constexpr uint64_t ns_to_90k(uint64_t ns)
{
	return (ns / 100000) * 9 + (ns % 100000) * 9 / 100000;
}

constexpr uint64_t ts90k_to_ns(uint64_t t)
{
	return (t / 9) * 100000 + (t % 9) * 100000 / 9;
}

} // namespace flash
