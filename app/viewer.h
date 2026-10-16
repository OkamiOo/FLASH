#pragma once

#include <flash/srt_link.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

// Visionneuse : une piste vidéo et des pistes son d'un émetteur, dans une fenêtre à part
// (plein écran au double-clic ou avec F11), le son sur la sortie choisie, à l'heure de l'image.

struct ViewerOptions {
	std::string title;              // titre de la fenêtre
	flash::SrtOptions srt;          // refresh : adresse et clé avant chaque tentative
	std::string video;              // piste vidéo par son nom ; vide : la première ; "-" : aucune
	std::vector<std::string> audio; // pistes son par leur nom ; "*" : toutes
	std::string output;             // sortie son (morceau de nom), vide : celle de Windows
	int screen = 0;                 // écran où ouvrir la fenêtre (0 : celui de la souris)
	bool fullscreen = false;
	bool background = false;        // ne prend pas la main sur le clavier (essais)
};

class Viewer {
public:
	explicit Viewer(const ViewerOptions &options);
	~Viewer();

	bool closed() const; // fenêtre fermée par l'utilisateur
	nlohmann::json status();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
