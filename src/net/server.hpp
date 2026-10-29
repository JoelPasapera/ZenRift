// Servidor HTTP/1.1 de un solo hilo sobre el bucle de eventos.
//
// - Conexiones en un array de ranuras con generación: el id (generación <<32 |
//   ranura) sirve de token del poller y de referencia segura para responder
//   más tarde; un id viejo nunca alcanza a una conexión nueva.
// - Cada conexión procesa una petición cada vez (pipelining en orden): si el
//   manejador difiere la respuesta, las siguientes esperan en el búfer.
// - Server-Sent Events: una conexión puede pasar a modo flujo y recibir
//   eventos indefinidamente; si el cliente no lee y el búfer de salida supera
//   el límite, se cierra (un consumidor lento no puede agotar la memoria).
// - Otros hilos solo pueden encolar tareas con post(): se ejecutan en este hilo.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/mpsc_queue.hpp"
#include "net/http.hpp"
#include "net/poller.hpp"
#include "net/socket.hpp"
#include "net/waker.hpp"

namespace zr::net {

struct ServerOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 7070;
  std::string poller;  // "", "epoll" o "poll"
  size_t max_connections = 10000;
  size_t max_body_bytes = 64u << 20;
  size_t max_header_bytes = 16 * 1024;
  int64_t idle_timeout_ms = 60000;
  int64_t header_timeout_ms = 15000;
  int64_t request_timeout_ms = 30000;
  int64_t sse_ping_ms = 15000;
  size_t max_output_bytes = 16u << 20;
  std::string server_header = "ZenRift";
  std::string cors_origin;  // vacío = sin CORS
};

struct ConnHandle {
  uint64_t id = 0;
  uint64_t seq = 0;
};

class HttpServer {
 public:
  using Handler = std::function<void(HttpServer&, const ConnHandle&, HttpRequest&)>;
  using CloseHook = std::function<void(uint64_t conn_id)>;

  explicit HttpServer(ServerOptions opts);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool listen(std::string& err);
  void set_handler(Handler h) { handler_ = std::move(h); }
  void set_close_hook(CloseHook h) { close_hook_ = std::move(h); }

  // Bucle principal; vuelve tras stop().
  void run();
  // Seguro entre hilos y desde un manejador de señales.
  void stop() noexcept;
  // Encola una tarea para el hilo del servidor (seguro entre hilos).
  void post(std::function<void()> task);

  // --- Solo desde el hilo del servidor (manejadores y tareas) ---
  void respond(const ConnHandle& h, HttpResponse r, bool head_only = false);
  void defer(const ConnHandle& h);
  bool start_sse(const ConnHandle& h);
  bool sse_send(uint64_t conn_id, std::string_view event, std::string_view data);
  void close(uint64_t conn_id);
  bool alive(uint64_t conn_id) const noexcept;

  uint16_t bound_port() const noexcept { return bound_port_; }
  size_t connections() const noexcept { return live_; }
  const char* poller_name() const noexcept { return poller_ ? poller_->name() : "?"; }
  const ServerOptions& options() const noexcept { return opts_; }

 private:
  enum class State : uint8_t { Reading, Waiting, Streaming, Closing };

  struct Conn {
    socket_t sock = kInvalidSocket;
    uint32_t gen = 0;
    bool used = false;
    State state = State::Reading;
    bool keep_alive = true;
    bool want_write = false;
    bool sent_continue = false;
    bool head_only = false;
    std::string in;
    std::string out;
    size_t out_off = 0;
    RequestParser parser;
    uint64_t seq = 0;
    uint64_t answered = 0;  // última petición respondida, diferida o convertida en SSE
    int64_t last_activity = 0;
    int64_t deadline = 0;
    int64_t last_ping = 0;
    std::string peer;
  };

  Conn* lookup(uint64_t id) noexcept;
  const Conn* lookup(uint64_t id) const noexcept;
  static uint64_t make_id(uint32_t slot, uint32_t gen) noexcept { return (static_cast<uint64_t>(gen) << 32) | slot; }

  void accept_all();
  void on_readable(uint64_t id, Conn& c);
  void process(uint64_t id, Conn& c);
  void try_write(uint64_t id, Conn& c);
  void update_interest(uint64_t id, Conn& c, bool want_write);
  void write_head(Conn& c, int status, std::string_view content_type, size_t length,
                  const std::vector<std::pair<std::string, std::string>>& extra, bool keep_alive);
  void sweep(int64_t now);
  void close_conn(uint64_t id, Conn& c);

  ServerOptions opts_;
  std::unique_ptr<Poller> poller_;
  Waker waker_;
  socket_t listener_ = kInvalidSocket;
  uint16_t bound_port_ = 0;
  std::vector<Conn> slots_;
  std::vector<uint32_t> free_;
  size_t live_ = 0;
  Handler handler_;
  CloseHook close_hook_;
  MpscQueue<std::function<void()>> tasks_;
  std::atomic<bool> running_{false};
  bool dispatching_ = false;
};

}  // namespace zr::net
