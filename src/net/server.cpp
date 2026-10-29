#include "net/server.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"

namespace zr::net {

namespace {
constexpr uint64_t kListenerToken = ~uint64_t{0};
constexpr uint64_t kWakerToken = ~uint64_t{0} - 1;
constexpr size_t kReadChunk = 64 * 1024;
}  // namespace

HttpServer::HttpServer(ServerOptions opts) : opts_(std::move(opts)) {
  slots_.reserve(opts_.max_connections);
}

HttpServer::~HttpServer() {
  for (size_t i = 0; i < slots_.size(); ++i)
    if (slots_[i].used) close_socket(slots_[i].sock);
  close_socket(listener_);
}

bool HttpServer::listen(std::string& err) {
  poller_ = Poller::create(opts_.poller, err);
  if (!poller_) return false;
  if (!waker_.open(err)) return false;
  listener_ = listen_tcp(opts_.host, opts_.port, err);
  if (listener_ == kInvalidSocket) return false;
  bound_port_ = local_port(listener_);
  poller_->add(listener_, kListenerToken, false);
  poller_->add(waker_.fd(), kWakerToken, false);
  return true;
}

void HttpServer::stop() noexcept {
  running_.store(false, std::memory_order_release);
  waker_.wake();
}

void HttpServer::post(std::function<void()> task) {
  // Solo quien encuentra la cola vacía avisa. Es correcto porque el bucle lee
  // el waker antes de vaciar la cola: si había trabajo, alguien escribió
  // después de dejarlo y ese aviso sigue pendiente o la tarea ya se recogió.
  // (Una bandera "aviso pendiente" aparte del contador del eventfd perdía
  // avisos: con la bandera a true y el contador ya leído, nadie volvía a
  // escribir y cada respuesta esperaba al plazo de 500 ms del poll.)
  if (tasks_.push(std::move(task))) waker_.wake();
}

HttpServer::Conn* HttpServer::lookup(uint64_t id) noexcept {
  const uint32_t slot = static_cast<uint32_t>(id & 0xffffffffu);
  const uint32_t gen = static_cast<uint32_t>(id >> 32);
  if (slot >= slots_.size()) return nullptr;
  Conn& c = slots_[slot];
  return c.used && c.gen == gen ? &c : nullptr;
}

const HttpServer::Conn* HttpServer::lookup(uint64_t id) const noexcept {
  return const_cast<HttpServer*>(this)->lookup(id);
}

bool HttpServer::alive(uint64_t conn_id) const noexcept { return lookup(conn_id) != nullptr; }

void HttpServer::run() {
  running_.store(true, std::memory_order_release);
  std::vector<PollEvent> events;
  std::vector<std::function<void()>> tasks;
  int64_t last_sweep = mono_ms();
  while (running_.load(std::memory_order_acquire)) {
    poller_->wait(events, 500);
    for (const auto& e : events) {
      if (e.token == kListenerToken) {
        accept_all();
        continue;
      }
      if (e.token == kWakerToken) {
        waker_.drain();
        continue;
      }
      Conn* c = lookup(e.token);
      if (!c) continue;
      if (e.readable || e.hangup) on_readable(e.token, *c);
      c = lookup(e.token);
      if (c && e.writable) try_write(e.token, *c);
    }
    if (tasks_.try_pop_all(tasks)) {
      for (auto& t : tasks) t();
      tasks.clear();
    }
    const int64_t now = mono_ms();
    if (now - last_sweep >= 500) {
      sweep(now);
      last_sweep = now;
    }
  }
  // Cierre: atender las tareas pendientes (respuestas en vuelo) y cerrar.
  if (tasks_.try_pop_all(tasks))
    for (auto& t : tasks) t();
  for (uint32_t i = 0; i < slots_.size(); ++i)
    if (slots_[i].used) close_conn(make_id(i, slots_[i].gen), slots_[i]);
}

void HttpServer::accept_all() {
  while (true) {
    std::string peer;
    const socket_t s = accept_conn(listener_, &peer);
    if (s == kInvalidSocket) return;
    if (live_ >= opts_.max_connections) {
      close_socket(s);
      log::warn("conexión rechazada: se alcanzó el máximo de ", opts_.max_connections);
      continue;
    }
    set_nonblocking(s);
    set_nodelay(s);
    uint32_t slot;
    if (!free_.empty()) {
      slot = free_.back();
      free_.pop_back();
    } else {
      slot = static_cast<uint32_t>(slots_.size());
      slots_.emplace_back();
    }
    Conn& c = slots_[slot];
    c.sock = s;
    c.used = true;
    c.state = State::Reading;
    c.keep_alive = true;
    c.want_write = false;
    c.sent_continue = false;
    c.in.clear();
    c.out.clear();
    c.out_off = 0;
    c.parser = RequestParser(RequestParser::Limits{opts_.max_header_bytes, opts_.max_body_bytes});
    c.last_activity = mono_ms();
    c.peer = std::move(peer);
    ++live_;
    poller_->add(s, make_id(slot, c.gen), false);
  }
}

void HttpServer::on_readable(uint64_t id, Conn& c) {
  for (int round = 0; round < 8; ++round) {
    const size_t old = c.in.size();
    c.in.resize(old + kReadChunk);
    const long n = recv_some(c.sock, c.in.data() + old, kReadChunk);
    if (n > 0) {
      c.in.resize(old + static_cast<size_t>(n));
      c.last_activity = mono_ms();
      if (static_cast<size_t>(n) < kReadChunk) break;
      continue;
    }
    c.in.resize(old);
    if (n == 0) {  // el cliente cerró
      close_conn(id, c);
      return;
    }
    const int err = last_error();
    if (would_block(err)) break;
    close_conn(id, c);
    return;
  }
  if (c.state == State::Streaming) {
    c.in.clear();  // en SSE no se esperan más peticiones
    return;
  }
  if (c.state == State::Reading) process(id, c);
}

void HttpServer::process(uint64_t id, Conn& c) {
  if (dispatching_) return;  // se reanuda al volver del manejador
  dispatching_ = true;
  while (c.used && c.state == State::Reading && !c.in.empty()) {
    HttpRequest req;
    size_t consumed = 0;
    const auto st = c.parser.parse(c.in, req, consumed);
    if (st == RequestParser::Status::NeedMore) {
      if (c.parser.headers_done() && c.parser.expects_continue() && !c.sent_continue) {
        c.out += "HTTP/1.1 100 Continue\r\n\r\n";
        c.sent_continue = true;
      }
      break;
    }
    if (st == RequestParser::Status::Error) {
      HttpResponse r = HttpResponse::error(c.parser.error_status(), c.parser.error_message());
      c.keep_alive = false;
      c.in.clear();
      ++c.seq;
      dispatching_ = false;
      respond(ConnHandle{id, c.seq}, std::move(r));
      return;
    }
    c.sent_continue = false;
    ++c.seq;
    c.keep_alive = req.keep_alive;
    c.head_only = req.method == "HEAD";
    req.peer = c.peer;
    const uint64_t seq = c.seq;
    if (handler_) handler_(*this, ConnHandle{id, seq}, req);
    Conn* cur = lookup(id);
    if (!cur) break;  // el manejador cerró la conexión
    cur->in.erase(0, consumed);
    if (cur->answered != seq) {
      // El manejador no respondió ni difirió: responder algo para no
      // desordenar las respuestas de las peticiones encadenadas.
      respond(ConnHandle{id, seq}, HttpResponse::error(500, "el manejador no produjo respuesta"));
    }
  }
  dispatching_ = false;
  if (Conn* cur = lookup(id)) try_write(id, *cur);
}

void HttpServer::write_head(Conn& c, int status, std::string_view content_type, size_t length,
                            const std::vector<std::pair<std::string, std::string>>& extra, bool keep_alive) {
  std::string& o = c.out;
  o += "HTTP/1.1 ";
  append_int(o, status);
  o += ' ';
  o += status_text(status);
  o += "\r\nServer: ";
  o += opts_.server_header;
  if (!content_type.empty()) {
    o += "\r\nContent-Type: ";
    o += content_type;
  }
  if (length != static_cast<size_t>(-1)) {
    o += "\r\nContent-Length: ";
    append_uint(o, length);
  }
  o += keep_alive ? "\r\nConnection: keep-alive" : "\r\nConnection: close";
  o += "\r\nX-Content-Type-Options: nosniff";
  if (!opts_.cors_origin.empty()) {
    o += "\r\nAccess-Control-Allow-Origin: ";
    o += opts_.cors_origin;
  }
  for (const auto& [k, v] : extra) {
    o += "\r\n";
    o += k;
    o += ": ";
    o += v;
  }
  o += "\r\n\r\n";
}

void HttpServer::respond(const ConnHandle& h, HttpResponse r, bool head_only) {
  Conn* c = lookup(h.id);
  if (!c || c->seq != h.seq || (c->state != State::Reading && c->state != State::Waiting)) return;
  c->answered = h.seq;
  const bool keep = c->keep_alive;
  const bool no_body = head_only || c->head_only || r.status == 204 || r.status == 304;
  write_head(*c, r.status, r.body.empty() && r.status == 204 ? std::string_view() : r.content_type, r.body.size(),
             r.headers, keep);
  if (!no_body) c->out += r.body;
  c->state = keep ? State::Reading : State::Closing;
  c->last_activity = mono_ms();
  try_write(h.id, *c);
  Conn* cur = lookup(h.id);
  // Respuesta diferida: si había peticiones encadenadas esperando, seguir.
  if (cur && cur->state == State::Reading && !cur->in.empty() && !dispatching_) process(h.id, *cur);
}

void HttpServer::defer(const ConnHandle& h) {
  Conn* c = lookup(h.id);
  if (!c || c->seq != h.seq) return;
  c->answered = h.seq;
  c->state = State::Waiting;
  c->deadline = mono_ms() + opts_.request_timeout_ms;
}

bool HttpServer::start_sse(const ConnHandle& h) {
  Conn* c = lookup(h.id);
  if (!c || c->seq != h.seq || c->state != State::Reading) return false;
  c->answered = h.seq;
  write_head(*c, 200, "text/event-stream; charset=utf-8", static_cast<size_t>(-1),
             {{"Cache-Control", "no-cache"}, {"X-Accel-Buffering", "no"}}, true);
  c->out += "retry: 2000\n\n";
  c->state = State::Streaming;
  c->last_ping = mono_ms();
  try_write(h.id, *c);
  return lookup(h.id) != nullptr;
}

bool HttpServer::sse_send(uint64_t conn_id, std::string_view event, std::string_view data) {
  Conn* c = lookup(conn_id);
  if (!c || c->state != State::Streaming) return false;
  if (c->out.size() - c->out_off > opts_.max_output_bytes) {
    log::warn("cliente SSE ", c->peer, " demasiado lento: se cierra la conexión");
    close_conn(conn_id, *c);
    return false;
  }
  if (!event.empty()) {
    c->out += "event: ";
    c->out += event;
    c->out += '\n';
  }
  // Cada línea del dato va en su propio "data:" (el JSON es de una línea).
  size_t start = 0;
  while (true) {
    const size_t nl = data.find('\n', start);
    c->out += "data: ";
    c->out.append(data.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
    c->out += '\n';
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  c->out += '\n';
  try_write(conn_id, *c);
  return lookup(conn_id) != nullptr;
}

void HttpServer::close(uint64_t conn_id) {
  if (Conn* c = lookup(conn_id)) close_conn(conn_id, *c);
}

void HttpServer::update_interest(uint64_t id, Conn& c, bool want_write) {
  if (c.want_write == want_write) return;
  c.want_write = want_write;
  poller_->modify(c.sock, id, want_write);
}

void HttpServer::try_write(uint64_t id, Conn& c) {
  while (c.out_off < c.out.size()) {
    const long n = send_some(c.sock, c.out.data() + c.out_off, c.out.size() - c.out_off);
    if (n > 0) {
      c.out_off += static_cast<size_t>(n);
      continue;
    }
    const int err = last_error();
    if (n < 0 && would_block(err)) {
      update_interest(id, c, true);
      // Compactar si lo enviado ya ocupa mucho.
      if (c.out_off > (1u << 20) && c.out_off * 2 > c.out.size()) {
        c.out.erase(0, c.out_off);
        c.out_off = 0;
      }
      return;
    }
    close_conn(id, c);
    return;
  }
  c.out.clear();
  c.out_off = 0;
  update_interest(id, c, false);
  if (c.state == State::Closing) close_conn(id, c);
}

void HttpServer::sweep(int64_t now) {
  for (uint32_t i = 0; i < slots_.size(); ++i) {
    Conn& c = slots_[i];
    if (!c.used) continue;
    const uint64_t id = make_id(i, c.gen);
    switch (c.state) {
      case State::Reading:
        if (!c.in.empty() && now - c.last_activity > opts_.header_timeout_ms) {
          c.keep_alive = false;
          ++c.seq;
          respond(ConnHandle{id, c.seq}, HttpResponse::error(408, "tiempo de espera agotado leyendo la petición"));
        } else if (c.in.empty() && c.out.empty() && now - c.last_activity > opts_.idle_timeout_ms) {
          close_conn(id, c);
        }
        break;
      case State::Waiting:
        if (now > c.deadline) {
          c.keep_alive = false;
          respond(ConnHandle{id, c.seq}, HttpResponse::error(504, "el motor no respondió a tiempo"));
        }
        break;
      case State::Streaming:
        if (now - c.last_ping >= opts_.sse_ping_ms) {
          c.last_ping = now;
          c.out += ": ping\n\n";
          try_write(id, c);
        }
        break;
      case State::Closing:
        if (now - c.last_activity > opts_.header_timeout_ms) close_conn(id, c);
        break;
    }
  }
}

void HttpServer::close_conn(uint64_t id, Conn& c) {
  if (!c.used) return;
  poller_->remove(c.sock);
  close_socket(c.sock);
  c.sock = kInvalidSocket;
  c.used = false;
  ++c.gen;
  c.in = std::string();
  c.out = std::string();
  c.out_off = 0;
  c.peer.clear();
  --live_;
  free_.push_back(static_cast<uint32_t>(id & 0xffffffffu));
  if (close_hook_) close_hook_(id);
}

}  // namespace zr::net
