#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace flash {

// Lecture du son sur une sortie de Windows, en 48 kHz stéréo flottant. On demande les
// échantillons au fur et à mesure en donnant l'heure (horloge commune) à laquelle le premier
// sera entendu : l'appelant y place ce qu'il veut faire entendre à cette heure-là.
class AudioOutput {
public:
	using Fill = std::function<void(float *samples, uint32_t frames, uint64_t first_ns)>;

	~AudioOutput();

	// device : morceau du nom de la sortie, vide = celle de Windows
	bool start(const std::string &device, Fill fill);
	void stop();

	// Écart entre l'écriture d'un échantillon et sa restitution (le plus grand récent)
	uint64_t latency_ns() const { return latency_; }
	std::string device_name() const { return name_; }

private:
	void run();

	std::string device_;
	std::string name_;
	Fill fill_;
	std::atomic<bool> running_{false};
	std::atomic<bool> ready_{false};
	std::atomic<bool> failed_{false};
	std::atomic<uint64_t> latency_{30'000'000};
	std::thread thread_;
};

} // namespace flash
