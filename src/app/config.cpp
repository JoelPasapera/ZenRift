#include "app/config.hpp"

#include <fstream>

#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"

namespace zr::app {

namespace {

bool as_bool(std::string_view v, bool& out) {
  if (iequals(v, "true") || iequals(v, "yes") || iequals(v, "on") || v == "1" || iequals(v, "si") ||
      iequals(v, "sí")) {
    out = true;
    return true;
  }
  if (iequals(v, "false") || iequals(v, "no") || iequals(v, "off") || v == "0") {
    out = false;
    return true;
  }
  return false;
}

}  // namespace

bool apply_setting(AppConfig& c, std::string_view key, std::string_view raw, std::string& err) {
  const std::string_view v = trim(raw);
  auto bad = [&](const char* what) {
    err = cat("valor inválido para '", key, "': '", v, "' (", what, ")");
    return false;
  };
  int64_t i = 0;
  uint64_t u = 0;
  int64_t ms = 0;
  double d = 0;
  std::string k(key);
  for (char& ch : k)
    if (ch == '-') ch = '_';

  if (k == "host") c.server.host = std::string(v);
  else if (k == "port") {
    if (!parse_int64(v, i) || i < 0 || i > 65535) return bad("entre 0 y 65535");
    c.server.port = static_cast<uint16_t>(i);
  } else if (k == "data_dir" || k == "data") c.engine.data_dir = std::string(v);
  else if (k == "shards") {
    if (!parse_int64(v, i) || i < 0 || i > 256) return bad("0 = automático, máx. 256");
    c.engine.shards = static_cast<uint32_t>(i);
  } else if (k == "fsync") {
    if (iequals(v, "always")) c.engine.fsync = engine::FsyncPolicy::Always;
    else if (iequals(v, "interval")) c.engine.fsync = engine::FsyncPolicy::Interval;
    else if (iequals(v, "never")) c.engine.fsync = engine::FsyncPolicy::Never;
    else return bad("always, interval o never");
  } else if (k == "fsync_interval") {
    if (!parse_duration(v, ms) || ms < 1) return bad("una duración como 1s");
    c.engine.fsync_interval_ms = ms;
  } else if (k == "segment_size") {
    if (!parse_size(v, u) || u < 4096) return bad("un tamaño como 64MB");
    c.engine.segment_bytes = u;
  } else if (k == "retention_bytes") {
    if (!parse_size(v, u)) return bad("un tamaño como 1GB; 0 = sin límite");
    c.engine.default_retention_bytes = u;
  } else if (k == "retention") {
    if (!parse_duration(v, ms)) return bad("una duración como 7d; 0s = sin límite");
    c.engine.default_retention_ms = ms;
  } else if (k == "partitions") {
    if (!parse_int64(v, i) || i < 1 || i > 64) return bad("entre 1 y 64");
    c.engine.default_partitions = static_cast<uint32_t>(i);
  } else if (k == "max_body") {
    if (!parse_size(v, u) || u < 1024) return bad("un tamaño como 64MB");
    c.server.max_body_bytes = u;
  } else if (k == "max_event_size") {
    if (!parse_size(v, u) || u < 64) return bad("un tamaño como 1MB");
    c.engine.max_event_bytes = u;
  } else if (k == "idle_timeout") {
    if (!parse_duration(v, ms)) return bad("una duración como 1s");
    c.engine.idle_ms = ms;
  } else if (k == "lateness") {
    if (!parse_duration(v, ms)) return bad("una duración como 2s");
    c.engine.compile.default_lateness_ms = ms;
  } else if (k == "window") {
    if (!parse_duration(v, ms) || ms < 100) return bad("una duración como 10s");
    c.engine.compile.default_window_ms = ms;
  } else if (k == "history") {
    if (!parse_int64(v, i) || i < 1 || i > 100000) return bad("número de ventanas");
    c.engine.history_windows = static_cast<size_t>(i);
  } else if (k == "max_groups") {
    if (!parse_int64(v, i) || i < 1) return bad("un número positivo");
    c.engine.max_groups = static_cast<size_t>(i);
  } else if (k == "max_rows") {
    if (!parse_int64(v, i) || i < 1) return bad("un número positivo");
    c.engine.max_rows = static_cast<size_t>(i);
  } else if (k == "recovery_horizon") {
    if (!parse_duration(v, ms)) return bad("una duración como 1h");
    c.engine.recovery_horizon_ms = ms;
  } else if (k == "auto_create_streams") {
    bool b;
    if (!as_bool(v, b)) return bad("true o false");
    c.engine.auto_create_streams = b;
  } else if (k == "tail_rate") {
    if (!parse_int64(v, i) || i < 1) return bad("eventos por segundo");
    c.engine.tail_max_events_per_sec = static_cast<uint32_t>(i);
  } else if (k == "max_future") {
    if (!parse_duration(v, ms)) return bad("una duración como 1h");
    c.engine.max_future_ms = ms;
  } else if (k == "auth_token") c.api.auth_token = std::string(v);
  else if (k == "cors_origin") c.server.cors_origin = std::string(v);
  else if (k == "web_dir") c.api.web_dir = std::string(v);
  else if (k == "poller") c.server.poller = std::string(v);
  else if (k == "max_connections") {
    if (!parse_int64(v, i) || i < 1) return bad("un número positivo");
    c.server.max_connections = static_cast<size_t>(i);
  } else if (k == "log_level") {
    log::Level lv;
    if (!log::parse_level(v, lv)) return bad("debug, info, warn, error u off");
    c.log_level = std::string(v);
  } else if (k == "demo") {
    bool b;
    if (!as_bool(v, b)) return bad("true o false");
    c.demo = b;
  } else if (k == "demo_rate") {
    if (!parse_double(v, d) || d <= 0 || d > 1e7) return bad("eventos por segundo");
    c.demo_rate = d;
  } else {
    err = cat("ajuste desconocido '", key, "' (ejecuta 'zenrift help' para ver la lista)");
    return false;
  }
  return true;
}

bool load_config_file(const std::string& path, AppConfig& cfg, std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = cat("no se pudo abrir el archivo de configuración ", path);
    return false;
  }
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    ++n;
    std::string_view l = trim(line);
    if (l.empty() || l[0] == '#' || l[0] == ';') continue;
    const size_t eq = l.find('=');
    if (eq == std::string_view::npos) {
      err = cat(path, ":", n, ": se esperaba 'clave = valor'");
      return false;
    }
    std::string_view value = trim(l.substr(eq + 1));
    // Comentario al final de la línea (fuera de comillas no hay comillas en este formato).
    if (const size_t hash = value.find(" #"); hash != std::string_view::npos) value = trim(value.substr(0, hash));
    std::string e;
    if (!apply_setting(cfg, trim(l.substr(0, eq)), value, e)) {
      err = cat(path, ":", n, ": ", e);
      return false;
    }
  }
  return true;
}

const char* settings_help() noexcept {
  return R"(Ajustes (archivo zenrift.conf o --clave valor en la línea de comandos):
  host               dirección de escucha                       [127.0.0.1]
  port               puerto HTTP                                 [7070]
  data_dir           directorio de datos                         [./data]
  shards             hilos de proceso (0 = núcleos - 1)          [0]
  fsync              always | interval | never                   [interval]
  fsync_interval     cada cuánto sincronizar a disco             [1s]
  segment_size       tamaño de los segmentos del log             [64MB]
  retention_bytes    tamaño máximo por partición (0 = sin límite) [1GB]
  retention          antigüedad máxima (0s = sin límite)         [7d]
  partitions         particiones de los streams nuevos           [1]
  max_body           tamaño máximo de una petición               [64MB]
  max_event_size     tamaño máximo de un evento                  [1MB]
  max_future         tolerancia de marcas de tiempo futuras      [1h]
  idle_timeout       inactividad tras la que avanza el reloj     [1s]
  window             ventana por defecto de las consultas        [10s]
  lateness           lateness por defecto                        [2s]
  history            ventanas guardadas por consulta             [360]
  max_groups         grupos por ventana y consulta               [100000]
  max_rows           filas por resultado si no hay LIMIT         [1000]
  recovery_horizon   historia que se re-procesa al arrancar      [1h]
  auto_create_streams crear streams al recibir eventos           [true]
  tail_rate          eventos/s máximos por tail en vivo          [500]
  auth_token         token Bearer para /v1 (vacío = sin auth)
  cors_origin        valor de Access-Control-Allow-Origin
  web_dir            servir el panel desde disco (desarrollo)
  poller             epoll | poll                                [automático]
  max_connections    conexiones simultáneas                      [10000]
  log_level          debug | info | warn | error | off           [info]
  demo               crear datos y consultas de demostración     [false]
  demo_rate          eventos/s del generador de la demo          [1500]
)";
}

}  // namespace zr::app
