// Registro de mensajes a stderr, seguro entre hilos.
//
//   2026-10-29T18:45:00.123Z INFO  [shard-1] partición requests/0 abierta
#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include "core/strings.hpp"

namespace zr::log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void set_level(Level lv) noexcept;
Level level() noexcept;
bool parse_level(std::string_view s, Level& out) noexcept;

// Nombre del hilo actual en las líneas de registro.
void set_thread_name(std::string_view name);

void write(Level lv, std::string_view msg);

inline bool enabled(Level lv) noexcept { return static_cast<int>(lv) >= static_cast<int>(level()); }

template <class... A>
void debug(const A&... a) {
  if (enabled(Level::Debug)) write(Level::Debug, cat(a...));
}
template <class... A>
void info(const A&... a) {
  if (enabled(Level::Info)) write(Level::Info, cat(a...));
}
template <class... A>
void warn(const A&... a) {
  if (enabled(Level::Warn)) write(Level::Warn, cat(a...));
}
template <class... A>
void error(const A&... a) {
  if (enabled(Level::Error)) write(Level::Error, cat(a...));
}

}  // namespace zr::log
