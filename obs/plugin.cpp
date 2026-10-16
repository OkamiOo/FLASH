#include "measure.h"

#include <flash/clock.h>
#include <flash/log.h>

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("flash-obs", "en-US")

void register_sources();

namespace {

void obs_log_sink(flash::LogLevel level, const char *message)
{
	int l = LOG_INFO;
	if (level == flash::LogLevel::Error)
		l = LOG_ERROR;
	else if (level == flash::LogLevel::Warning)
		l = LOG_WARNING;
	else if (level == flash::LogLevel::Debug)
		l = LOG_DEBUG;
	blog(l, "[FLASH] %s", message);
}

} // namespace

bool obs_module_load(void)
{
	flash::set_log_sink(obs_log_sink);
	flash::enable_precise_timers();
	register_sources();
	blog(LOG_INFO, "[FLASH] version %s", FLASH_VERSION);
	if (measure::enabled())
		blog(LOG_INFO, "[FLASH] journal de mesure actif");
	return true;
}

void obs_module_unload(void)
{
	measure::shutdown();
	flash::shutdown_log();
}

const char *obs_module_name(void)
{
	return "FLASH";
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}
