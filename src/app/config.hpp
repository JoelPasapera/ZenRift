// Configuración de la aplicación: archivo "clave = valor" y opciones de línea
// de comandos con los mismos nombres (--port 8080 equivale a port = 8080).
#pragma once

#include <string>
#include <string_view>

#include "api/api.hpp"
#include "engine/types.hpp"
#include "net/server.hpp"

namespace zr::app {

struct AppConfig {
  net::ServerOptions server;
  engine::EngineConfig engine;
  api::ApiOptions api;
  bool demo = false;
  double demo_rate = 1500;  // eventos por segundo del generador de la demo
  std::string log_level = "info";
};

// Aplica un ajuste por nombre. Devuelve false con un mensaje si no es válido.
bool apply_setting(AppConfig& cfg, std::string_view key, std::string_view value, std::string& err);
bool load_config_file(const std::string& path, AppConfig& cfg, std::string& err);
// Texto de ayuda con todas las claves.
const char* settings_help() noexcept;

}  // namespace zr::app
