// API HTTP de ZenRift: rutas REST, Server-Sent Events y el panel web.
//
// Es el puente entre el servidor (un hilo) y el motor (varios hilos): traduce
// peticiones a operaciones del motor y, como EngineListener, recibe sus
// respuestas y publicaciones desde otros hilos y las reencola al hilo del
// servidor, que es el único que toca las conexiones y las suscripciones.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/engine.hpp"
#include "net/router.hpp"
#include "net/server.hpp"

namespace zr::api {

struct ApiOptions {
  std::string auth_token;  // vacío = sin autenticación
  std::string web_dir;     // vacío = panel incrustado
};

class Api final : public engine::EngineListener {
 public:
  Api(net::HttpServer& server, ApiOptions opts);

  void attach(engine::Engine& engine);

  // EngineListener (cualquier hilo).
  void reply(const engine::ReplyTo& to, engine::Response r) override;
  void publish(engine::Publication p) override;

 private:
  void handle(net::HttpServer& s, const net::ConnHandle& h, net::HttpRequest& req);
  void install_routes();
  bool authorized(const net::HttpRequest& req) const;
  void serve_static(net::HttpServer& s, const net::ConnHandle& h, net::HttpRequest& req);
  void on_close(uint64_t conn);
  void fan_out(const engine::Publication& p);
  static engine::ReplyTo reply_to(const net::ConnHandle& h) { return engine::ReplyTo{h.id, h.seq}; }

  net::HttpServer& server_;
  ApiOptions opts_;
  engine::Engine* engine_ = nullptr;
  net::Router router_;
  // Suscripciones SSE (solo hilo del servidor).
  std::unordered_map<std::string, std::vector<uint64_t>> query_subs_;
  std::vector<uint64_t> live_subs_;
  std::unordered_map<uint64_t, uint64_t> tail_of_conn_;
  std::unordered_map<uint64_t, uint64_t> conn_of_tail_;
  uint64_t next_tail_ = 1;
};

}  // namespace zr::api
