// zenrift serve: arranca el motor, la API y el panel.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#include "api/api.hpp"
#include "app/commands.hpp"
#include "app/config.hpp"
#include "app/traffic.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "engine/engine.hpp"
#include "net/server.hpp"

namespace zr::app {

namespace {

net::HttpServer* g_server = nullptr;

extern "C" void on_signal(int) {
  if (g_server) g_server->stop();
}

// Consultas de la demo: cada una enseña una parte distinta del lenguaje.
struct DemoQuery {
  const char* name;
  const char* zql;
};

constexpr DemoQuery kDemoQueries[] = {
    {"trafico_por_servicio",
     "-- Volumen, latencia y errores por servicio cada 10 s, con parciales cada segundo\n"
     "SELECT service,\n"
     "       count(*)                  AS peticiones,\n"
     "       rate()                    AS rps,\n"
     "       p50(latency_ms)           AS p50,\n"
     "       p99(latency_ms)           AS p99,\n"
     "       avg(status >= 500) * 100  AS error_pct\n"
     "FROM requests\n"
     "GROUP BY service\n"
     "WINDOW TUMBLING 10s\n"
     "ORDER BY peticiones DESC\n"
     "EMIT EVERY 1s"},
    {"latencia_global",
     "-- Cuantiles de latencia de todo el sistema en una ventana deslizante de 1 minuto\n"
     "SELECT p50(latency_ms) AS p50,\n"
     "       p90(latency_ms) AS p90,\n"
     "       p99(latency_ms) AS p99,\n"
     "       max(latency_ms) AS maximo,\n"
     "       rate()          AS rps\n"
     "FROM requests\n"
     "WINDOW HOPPING 1m EVERY 5s"},
    {"rutas_mas_lentas",
     "-- Las 5 rutas con peor p99 en los últimos 30 s (descartando las de poco tráfico)\n"
     "SELECT route, p99(latency_ms) AS p99, count(*) AS n\n"
     "FROM requests\n"
     "GROUP BY route\n"
     "WINDOW TUMBLING 30s\n"
     "HAVING n >= 20\n"
     "ORDER BY p99 DESC\n"
     "LIMIT 5\n"
     "EMIT EVERY 2s"},
    {"usuarios_por_region",
     "-- Usuarios únicos (HyperLogLog) y plataformas más usadas (Space-Saving) por región\n"
     "SELECT region,\n"
     "       distinct(user)                  AS usuarios,\n"
     "       topk(client.platform, 3)        AS plataformas,\n"
     "       sum(bytes) / 1048576            AS mb_servidos\n"
     "FROM requests\n"
     "GROUP BY region\n"
     "WINDOW TUMBLING 1m\n"
     "EMIT EVERY 2s"},
    {"alerta_errores",
     "-- Alerta si un servicio supera el 5 % de errores 5xx durante 2 ventanas seguidas\n"
     "SELECT service, avg(status >= 500) * 100 AS error_pct, count(*) AS peticiones\n"
     "FROM requests\n"
     "GROUP BY service\n"
     "WINDOW TUMBLING 10s\n"
     "ALERT IF error_pct > 5 AND peticiones > 30 FOR 2 WINDOWS"},
    {"alerta_latencia_pagos",
     "-- Alerta de latencia en pagos: p95 por encima de 1,2 s\n"
     "SELECT p95(latency_ms) AS p95, count(*) AS n\n"
     "FROM requests\n"
     "WHERE service = 'payments'\n"
     "WINDOW TUMBLING 10s\n"
     "ALERT IF p95 > 1200 AND n > 10"},
};

void run_demo(engine::Engine& eng, double rate, std::atomic<bool>& stop) {
  log::set_thread_name("demo");
  TrafficGenerator gen(static_cast<uint64_t>(wall_ms()));
  constexpr int64_t kStepMs = 100;
  double carry = 0;
  std::string batch;
  auto next = std::chrono::steady_clock::now();
  while (!stop.load(std::memory_order_relaxed)) {
    next += std::chrono::milliseconds(kStepMs);
    carry += rate * kStepMs / 1000.0;
    const size_t n = static_cast<size_t>(carry);
    carry -= static_cast<double>(n);
    if (n > 0) {
      batch.clear();
      gen.generate(n, wall_ms(), kStepMs, batch);
      eng.ingest("requests", std::move(batch), engine::ReplyTo{});
      batch = std::string();
    }
    std::this_thread::sleep_until(next);
  }
}

}  // namespace

int cmd_serve(const std::vector<std::string>& args) {
  AppConfig cfg;
  std::string err;
  // Primero el archivo de configuración (si se indica), luego las opciones.
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--config" || args[i] == "-c") {
      if (!load_config_file(args[i + 1], cfg, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
      }
    }
  for (size_t i = 0; i < args.size(); ++i) {
    std::string a = args[i];
    if (a == "--config" || a == "-c") {
      ++i;
      continue;
    }
    if (a == "--demo") {
      cfg.demo = true;
      continue;
    }
    if (!a.starts_with("--")) {
      std::fprintf(stderr, "error: argumento inesperado '%s'\n", a.c_str());
      return 2;
    }
    a = a.substr(2);
    std::string value;
    if (const size_t eq = a.find('='); eq != std::string::npos) {
      value = a.substr(eq + 1);
      a = a.substr(0, eq);
    } else if (i + 1 < args.size()) {
      value = args[++i];
    } else {
      std::fprintf(stderr, "error: falta el valor de --%s\n", a.c_str());
      return 2;
    }
    if (!apply_setting(cfg, a, value, err)) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 2;
    }
  }
  log::Level lv;
  log::parse_level(cfg.log_level, lv);
  log::set_level(lv);

  if (!net::net_init(err)) {
    log::error(err);
    return 1;
  }
  cfg.server.server_header = cat("ZenRift/", ZENRIFT_VERSION);
  // Un evento nunca puede ser mayor que la petición que lo trae: el límite de
  // cuerpo que pidió el usuario manda y acota también el de evento.
  cfg.engine.max_event_bytes = std::min<size_t>(cfg.engine.max_event_bytes, cfg.server.max_body_bytes);
  net::HttpServer server(cfg.server);
  api::Api api(server, cfg.api);
  engine::Engine eng(cfg.engine, &api);
  if (!eng.start(err)) {
    log::error("no se pudo arrancar el motor: ", err);
    return 1;
  }
  api.attach(eng);
  if (!server.listen(err)) {
    log::error(err);
    eng.stop();
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::string host = cfg.server.host == "0.0.0.0" || cfg.server.host.empty() ? "localhost" : cfg.server.host;
  log::info("ZenRift ", ZENRIFT_VERSION, " escuchando en http://", host, ":", server.bound_port(), "  (", eng.shard_count(),
            " shard(s), ", server.poller_name(), ", datos en ", cfg.engine.data_dir, ")");
  if (!cfg.api.auth_token.empty()) log::info("autenticación activada: las rutas /v1 piden el token");

  std::atomic<bool> demo_stop{false};
  std::thread demo;
  if (cfg.demo) {
    engine::StreamConfig sc;
    sc.name = "requests";
    sc.partitions = std::max<uint32_t>(2, eng.shard_count());
    eng.create_stream(sc, engine::ReplyTo{});
    for (const auto& q : kDemoQueries) eng.create_query(q.name, q.zql, "now", engine::ReplyTo{});
    demo = std::thread(run_demo, std::ref(eng), cfg.demo_rate, std::ref(demo_stop));
    log::info("modo demo: ", cfg.demo_rate, " eventos/s sintéticos en el stream 'requests' y ",
              sizeof kDemoQueries / sizeof kDemoQueries[0], " consultas de ejemplo");
  }

  server.run();

  log::info("deteniendo...");
  demo_stop = true;
  if (demo.joinable()) demo.join();
  eng.stop();
  g_server = nullptr;
  log::info("ZenRift detenido; los datos están en ", cfg.engine.data_dir);
  return 0;
}

}  // namespace zr::app
