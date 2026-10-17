#pragma once

#include <flash/sender.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Lien avec le plugin OBS de ce PC (voir CONCEPTION.md, § 15) : liste des scènes et sources,
// aperçus, et entrées vidéo et son pour l'envoi. Les entrées attendent qu'OBS soit ouvert et
// reprennent seules quand il revient.

class ObsClient {
public:
	using OnCatalog = std::function<void(const nlohmann::json &catalog)>; // {connecte, scenes, sources}
	using OnPreview = std::function<void(const std::string &key, const std::vector<uint8_t> &jpeg)>;

	explicit ObsClient(flash::ComPtr<ID3D11Device> device);
	~ObsClient();

	void set_listener(OnCatalog on_catalog, OnPreview on_preview);
	nlohmann::json catalog();
	void set_previews(bool on);

	// kind : « obs-scene », « obs-source » ou « obs-programme »
	std::unique_ptr<flash::VideoInput> video_input(const std::string &kind, const std::string &name, uint32_t max_w,
						       uint32_t max_h);
	std::unique_ptr<flash::AudioInput> audio_input(const std::string &name);

	struct Impl;

private:
	std::shared_ptr<Impl> impl_;
};
