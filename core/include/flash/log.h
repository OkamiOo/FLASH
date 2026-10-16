#pragma once

namespace flash {

enum class LogLevel { Debug, Info, Warning, Error };

// Le programme hôte (appli ou plugin OBS) décide où vont les messages.
using LogSink = void (*)(LogLevel level, const char *message);

void set_log_sink(LogSink sink);
void log(LogLevel level, const char *format, ...);
// Vide la file des messages et arrête le fil d'écriture (fin de programme, déchargement).
void shutdown_log();

} // namespace flash

#define FLOG_D(...) ::flash::log(::flash::LogLevel::Debug, __VA_ARGS__)
#define FLOG_I(...) ::flash::log(::flash::LogLevel::Info, __VA_ARGS__)
#define FLOG_W(...) ::flash::log(::flash::LogLevel::Warning, __VA_ARGS__)
#define FLOG_E(...) ::flash::log(::flash::LogLevel::Error, __VA_ARGS__)
