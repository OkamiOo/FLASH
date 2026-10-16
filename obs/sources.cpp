#include "connection.h"
#include "measure.h"

#include <obs-module.h>

#include <memory>
#include <string>

namespace {

struct FlashSource {
	obs_source_t *source = nullptr;
	bool video = false;
	std::shared_ptr<Connection> conn;
	std::string address;
	int port = 0;
	int track = 0;
};

void source_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<FlashSource *>(data);
	const std::string address = obs_data_get_string(settings, "address");
	const int port = (int)obs_data_get_int(settings, "port");
	const int track = (int)obs_data_get_int(settings, "track");
	if (s->conn && address == s->address && port == s->port && track == s->track)
		return;

	if (s->conn)
		s->conn->remove_source(s->source);
	s->address = address;
	s->port = port;
	s->track = track;
	s->conn = Connection::get(address, (uint16_t)port);
	s->conn->add_source(s->source, s->video, track);
}

void *source_create(obs_data_t *settings, obs_source_t *source, bool video)
{
	auto *s = new FlashSource;
	s->source = source;
	s->video = video;
	if (video) {
		// FLASH décide lui-même de l'heure de chaque image : OBS l'affiche dès réception
		obs_source_set_async_unbuffered(source, true);
		obs_source_set_async_decoupled(source, true);
	}
	source_update(s, settings);
	return s;
}

void source_destroy(void *data)
{
	auto *s = static_cast<FlashSource *>(data);
	if (s->conn)
		s->conn->remove_source(s->source);
	delete s;
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "address", "127.0.0.1");
	obs_data_set_default_int(settings, "port", 9710);
	obs_data_set_default_int(settings, "track", 1);
}

obs_properties_t *source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "address", obs_module_text("Address"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", obs_module_text("Port"), 1, 65535, 1);
	obs_properties_add_int(props, "track", obs_module_text("Track"), 1, 16, 1);
	return props;
}

// Mesure : à chaque rendu, l'image affichée est la dernière remise à la source
void video_tick(void *data, float)
{
	auto *s = static_cast<FlashSource *>(data);
	measure::tick(s->source, obs_get_video_frame_time());
}

} // namespace

void register_sources()
{
	struct obs_source_info video = {};
	video.id = "flash_video_source";
	video.type = OBS_SOURCE_TYPE_INPUT;
	video.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	video.icon_type = OBS_ICON_TYPE_MEDIA;
	video.get_name = [](void *) { return obs_module_text("VideoSource"); };
	video.create = [](obs_data_t *settings, obs_source_t *source) { return source_create(settings, source, true); };
	video.destroy = source_destroy;
	video.update = source_update;
	video.get_defaults = source_defaults;
	video.get_properties = source_properties;
	video.video_tick = video_tick;
	obs_register_source(&video);

	struct obs_source_info audio = {};
	audio.id = "flash_audio_source";
	audio.type = OBS_SOURCE_TYPE_INPUT;
	audio.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	audio.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	audio.get_name = [](void *) { return obs_module_text("AudioSource"); };
	audio.create = [](obs_data_t *settings, obs_source_t *source) { return source_create(settings, source, false); };
	audio.destroy = source_destroy;
	audio.update = source_update;
	audio.get_defaults = source_defaults;
	audio.get_properties = source_properties;
	obs_register_source(&audio);
}
