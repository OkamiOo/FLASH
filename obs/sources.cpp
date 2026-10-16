#include "connection.h"
#include "directory.h"

#include <flash/measure.h>
#include <flash/pairing.h>

#include <obs-module.h>

#include <memory>
#include <string>

namespace {

struct FlashSource {
	obs_source_t *source = nullptr;
	bool video = false;
	std::shared_ptr<Connection> conn;
	Target target;
	int track = 0;
	std::string track_name;
	std::string status; // dernier message de l'appairage
};

Target target_from(obs_data_t *settings)
{
	Target t;
	t.sender_id = obs_data_get_string(settings, "emetteur");
	t.address = obs_data_get_string(settings, "address");
	t.port = (uint16_t)obs_data_get_int(settings, "port");
	t.latency_ms = (int)obs_data_get_int(settings, "latence");
	return t;
}

void source_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<FlashSource *>(data);
	const Target t = target_from(settings);
	const std::string name = obs_data_get_string(settings, "piste");
	const int track = (int)obs_data_get_int(settings, "track");
	if (s->conn && t.key() == s->target.key() && t.latency_ms == s->target.latency_ms && name == s->track_name &&
	    track == s->track)
		return;

	if (s->conn)
		s->conn->remove_source(s->source);
	s->conn.reset();
	s->target = t;
	s->track = track;
	s->track_name = name;
	if (t.sender_id.empty() && t.address.empty())
		return; // rien de choisi pour l'instant
	s->conn = Connection::get(t);
	s->conn->add_source(s->source, s->video, track, name);
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
	obs_data_set_default_string(settings, "emetteur", "");
	obs_data_set_default_string(settings, "address", "");
	obs_data_set_default_int(settings, "port", 9710);
	obs_data_set_default_int(settings, "track", 1);
	obs_data_set_default_int(settings, "latence", 20);
}

// ---------- Propriétés ----------

// Remplit la liste des pistes de l'émetteur choisi, et affiche ce qui dépend de l'appairage
void refresh_dependent(FlashSource *s, obs_properties_t *props, obs_data_t *settings)
{
	const std::string id = obs_data_get_string(settings, "emetteur");
	const bool manual = id.empty();
	obs_property_set_visible(obs_properties_get(props, "address"), manual);
	obs_property_set_visible(obs_properties_get(props, "port"), manual);

	const auto found = manual ? std::nullopt : directory::find(id, false);
	const bool paired = !manual && directory::paired_with(id).has_value();
	const bool needs_code = !manual && !paired && !(found && found->open);
	obs_property_set_visible(obs_properties_get(props, "code"), needs_code);
	obs_property_set_visible(obs_properties_get(props, "appairer"), needs_code);

	obs_property_t *list = obs_properties_get(props, "piste");
	obs_property_list_clear(list);
	const std::string current = obs_data_get_string(settings, "piste");
	bool has_current = current.empty();
	if (found) {
		for (const flash::AnnouncedTrack &t : found->tracks) {
			if (t.video != s->video)
				continue;
			obs_property_list_add_string(list, t.name.c_str(), t.name.c_str());
			has_current = has_current || t.name == current;
		}
	}
	if (!has_current)
		obs_property_list_add_string(list, current.c_str(), current.c_str()); // émetteur absent pour l'instant
	if (obs_property_list_item_count(list) == 0)
		obs_property_list_add_string(list, obs_module_text("FirstTrack"), "");

	std::string status = s->status;
	if (status.empty()) {
		if (manual)
			status = obs_module_text("StatusManual");
		else if (!found)
			status = obs_module_text("StatusOffline");
		else if (paired)
			status = s->conn && s->conn->connected() ? obs_module_text("StatusConnected") : obs_module_text("StatusPaired");
		else if (found->open)
			status = obs_module_text("StatusOpen");
		else
			status = obs_module_text("StatusNeedsCode");
	}
	obs_property_set_description(obs_properties_get(props, "etat"), status.c_str());
}

bool on_sender_changed(void *data, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	auto *s = static_cast<FlashSource *>(data);
	if (!s)
		return false;
	s->status.clear();
	refresh_dependent(s, props, settings);
	return true;
}

bool on_search(obs_properties_t *, obs_property_t *, void *)
{
	directory::senders(true);
	return true; // OBS redemande les propriétés : la liste est refaite
}

bool on_pair(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *s = static_cast<FlashSource *>(data);
	if (!s)
		return false;
	obs_data_t *settings = obs_source_get_settings(s->source);
	const std::string id = obs_data_get_string(settings, "emetteur");
	const std::string code = obs_data_get_string(settings, "code");
	const auto found = directory::find(id, false);
	if (!found) {
		s->status = obs_module_text("StatusOffline");
	} else if (code.empty()) {
		s->status = obs_module_text("StatusNeedsCode");
	} else {
		const flash::PairingResult r =
			flash::pair_with_sender(found->address, found->pairing_port, code, {directory::self_id(), directory::self_name()});
		if (r.ok) {
			directory::remember({r.sender.id, r.sender.name, r.key_hex});
			obs_data_set_string(settings, "code", "");
			s->status = obs_module_text("StatusPairedNow");
			// La connexion repart avec la clé
			if (s->conn)
				s->conn->remove_source(s->source);
			s->conn.reset();
			s->target = {};
			source_update(s, settings);
		} else {
			s->status = r.error;
		}
	}
	refresh_dependent(s, props, settings);
	obs_data_release(settings);
	return true;
}

obs_properties_t *source_properties(void *data)
{
	auto *s = static_cast<FlashSource *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *sender = obs_properties_add_list(props, "emetteur", obs_module_text("Sender"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	for (const flash::SenderAnnounce &a : directory::senders(false))
		obs_property_list_add_string(sender, a.name.c_str(), a.id.c_str());
	for (const directory::Paired &p : directory::paired()) {
		if (!directory::find(p.id, false)) {
			const std::string label = p.name + " " + obs_module_text("Offline");
			obs_property_list_add_string(sender, label.c_str(), p.id.c_str());
		}
	}
	obs_property_list_add_string(sender, obs_module_text("ManualAddress"), "");
	obs_property_set_modified_callback2(sender, on_sender_changed, s);
	obs_properties_add_button2(props, "chercher", obs_module_text("Search"), on_search, nullptr);

	obs_properties_add_text(props, "address", obs_module_text("Address"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", obs_module_text("Port"), 1, 65535, 1);

	obs_properties_add_list(props, "piste", obs_module_text("Track"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_properties_add_text(props, "code", obs_module_text("Code"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(props, "appairer", obs_module_text("Pair"), on_pair, s);
	obs_properties_add_text(props, "etat", "", OBS_TEXT_INFO);

	obs_property_t *latency = obs_properties_add_list(props, "latence", obs_module_text("Network"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(latency, obs_module_text("NetworkCable"), 20);
	obs_property_list_add_int(latency, obs_module_text("NetworkWifi"), 60);
	obs_property_list_add_int(latency, obs_module_text("NetworkInternet"), 200);

	// Sans instance (propriétés demandées pour le type de source seul), rien à remplir
	if (s) {
		obs_data_t *settings = obs_source_get_settings(s->source);
		refresh_dependent(s, props, settings);
		obs_data_release(settings);
	}
	return props;
}

// Mesure : à chaque rendu, l'image affichée est la dernière remise à la source
void video_tick(void *data, float)
{
	auto *s = static_cast<FlashSource *>(data);
	flash::measure::tick(s->source, obs_get_video_frame_time());
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
