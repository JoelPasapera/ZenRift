// Pruebas del parser HTTP, del enrutador y del servidor (con sockets reales).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/strings.hpp"
#include "core/time.hpp"
#include "framework.hpp"
#include "net/http.hpp"
#include "net/http_client.hpp"
#include "net/router.hpp"
#include "net/server.hpp"
#include "net/socket.hpp"

using namespace zr;
using namespace zr::net;

namespace {

struct Parsed {
  RequestParser::Status status;
  HttpRequest req;
  size_t consumed = 0;
  std::string body;
};

// Alimenta el parser byte a byte hasta completar (simula lecturas parciales).
Parsed feed_bytewise(RequestParser& p, const std::string& data) {
  Parsed out;
  for (size_t n = 1; n <= data.size(); ++n) {
    out.status = p.parse(std::string_view(data).substr(0, n), out.req, out.consumed);
    if (out.status != RequestParser::Status::NeedMore) break;
  }
  if (out.status == RequestParser::Status::Done) out.body = std::string(out.req.body);
  return out;
}

}  // namespace

TEST(http_parser_peticion_simple_y_keep_alive) {
  RequestParser p;
  const std::string raw = "GET /v1/streams?x=1&nombre=a%20b+c HTTP/1.1\r\nHost: h\r\nX-Algo:  valor  \r\n\r\n";
  HttpRequest req;
  size_t consumed = 0;
  REQUIRE(p.parse(raw, req, consumed) == RequestParser::Status::Done);
  CHECK_EQ(consumed, raw.size());
  CHECK_EQ(req.method, std::string_view("GET"));
  CHECK_EQ(req.path, std::string_view("/v1/streams"));
  CHECK_EQ(req.header("x-algo"), std::string_view("valor"));
  CHECK(req.keep_alive);
  CHECK_EQ(req.query_param("nombre"), std::string("a b c"));
  CHECK_EQ(req.query_param("falta", "def"), std::string("def"));
  CHECK(req.has_query("x"));
  HttpRequest r10;
  RequestParser p2;
  REQUIRE(p2.parse("GET / HTTP/1.0\r\n\r\n", r10, consumed) == RequestParser::Status::Done);
  CHECK(!r10.keep_alive);
}

TEST(http_parser_cuerpo_y_lecturas_parciales) {
  RequestParser p;
  const std::string raw = "POST /e HTTP/1.1\r\nContent-Length: 11\r\n\r\nhola mundo!";
  const Parsed r = feed_bytewise(p, raw);
  REQUIRE(r.status == RequestParser::Status::Done);
  CHECK_EQ(r.body, std::string("hola mundo!"));
  CHECK_EQ(r.consumed, raw.size());
}

TEST(http_parser_chunked_con_extensiones_y_trailers) {
  RequestParser p;
  const std::string raw =
      "POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5;ext=1\r\nhola \r\n6\r\nmundo!\r\n0\r\nX-Trailer: si\r\n\r\nGET /siguiente HTTP/1.1\r\n\r\n";
  const Parsed r = feed_bytewise(p, raw);
  REQUIRE(r.status == RequestParser::Status::Done);
  CHECK_EQ(r.body, std::string("hola mundo!"));
  // La petición encadenada empieza justo donde acabó la anterior.
  const std::string rest = raw.substr(r.consumed);
  HttpRequest next;
  size_t consumed = 0;
  REQUIRE(p.parse(rest, next, consumed) == RequestParser::Status::Done);
  CHECK_EQ(next.path, std::string_view("/siguiente"));
}

TEST(http_parser_rechaza_entradas_peligrosas) {
  struct Case {
    std::string raw;
    int status;
  };
  RequestParser::Limits lim;
  lim.max_header_bytes = 256;
  lim.max_body_bytes = 100;
  const Case cases[] = {
      {"GARBAGE\r\n\r\n", 400},
      {"GET / HTTP/2.0\r\n\r\n", 505},
      {"GET http://x/ HTTP/1.1\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\nX: " + std::string(400, 'a') + "\r\n\r\n", 431},
      {"POST / HTTP/1.1\r\nContent-Length: 101\r\n\r\n", 413},
      {"POST / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
      {"POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", 501},
      {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n", 400},
      {"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\n folded\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", 400},
  };
  for (const auto& c : cases) {
    RequestParser p(lim);
    HttpRequest req;
    size_t consumed = 0;
    const auto st = p.parse(c.raw, req, consumed);
    if (st != RequestParser::Status::Error || p.error_status() != c.status)
      zt::report_failure(__FILE__, __LINE__,
                         cat("caso '", c.raw.substr(0, 40), "': estado ", static_cast<int>(st), " código ",
                             p.error_status()));
  }
}

TEST(router_parametros_y_metodos) {
  Router r;
  int hit = 0;
  r.add("GET", "/v1/streams/{name}/events", [&](HttpServer&, const ConnHandle&, HttpRequest&) { hit = 1; });
  r.add("POST", "/v1/streams/{name}/events", [&](HttpServer&, const ConnHandle&, HttpRequest&) { hit = 2; });
  HttpRequest req;
  req.method = "GET";
  req.path = "/v1/streams/mi%2Dstream/events";
  const Router::Handler* h = nullptr;
  std::string allowed;
  REQUIRE(r.match(req, h, allowed) == Router::Match::Found);
  CHECK_EQ(req.param("name"), std::string_view("mi-stream"));
  req.method = "DELETE";
  CHECK(r.match(req, h, allowed) == Router::Match::MethodNotAllowed);
  CHECK_EQ(allowed, std::string("GET, POST"));
  req.path = "/v1/otra";
  CHECK(r.match(req, h, allowed) == Router::Match::NotFound);
  req.method = "HEAD";
  req.path = "/v1/streams/x/events";
  CHECK(r.match(req, h, allowed) == Router::Match::Found);
}

namespace {

// Servidor real en un hilo, en un puerto efímero.
struct LiveServer {
  ServerOptions opts;
  std::unique_ptr<HttpServer> server;
  std::thread thread;
  uint16_t port = 0;

  explicit LiveServer(HttpServer::Handler handler, ServerOptions o = {}) : opts(std::move(o)) {
    std::string err;
    net_init(err);
    opts.host = "127.0.0.1";
    opts.port = 0;
    server = std::make_unique<HttpServer>(opts);
    server->set_handler(std::move(handler));
    if (!server->listen(err)) zt::report_failure(__FILE__, __LINE__, "listen: " + err);
    port = server->bound_port();
    thread = std::thread([this] { server->run(); });
  }
  ~LiveServer() {
    server->stop();
    thread.join();
  }

  // Envía bytes crudos y lee hasta que el servidor cierre o pase el plazo.
  std::string raw(const std::string& data, int wait_ms = 300) const {
    std::string err;
    socket_t s = connect_tcp("127.0.0.1", port, err);
    if (s == kInvalidSocket) return "ERROR " + err;
    send_some(s, data.data(), data.size());
    std::string out;
    set_nonblocking(s);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    char buf[4096];
    while (std::chrono::steady_clock::now() < end) {
      const long n = recv_some(s, buf, sizeof buf);
      if (n > 0) out.append(buf, static_cast<size_t>(n));
      else if (n == 0) break;
      else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    close_socket(s);
    return out;
  }
};

size_t count_of(const std::string& s, std::string_view sub) {
  size_t n = 0;
  for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) ++n;
  return n;
}

}  // namespace

TEST(servidor_keep_alive_pipelining_y_respuestas_diferidas) {
  HttpServer* srv = nullptr;
  LiveServer ls([&srv](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    srv = &s;
    if (req.path == "/lento") {
      // Respuesta desde otro hilo, pasando por post().
      s.defer(h);
      std::thread([&s, h] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        s.post([&s, h] { s.respond(h, HttpResponse::text(200, "lento")); });
      }).detach();
      return;
    }
    s.respond(h, HttpResponse::text(200, cat("eco:", req.path, ":", req.body)));
  });
  // Cliente con keep-alive: varias peticiones por la misma conexión.
  HttpClient client("127.0.0.1", ls.port);
  for (int i = 0; i < 20; ++i) {
    ClientResponse r;
    std::string err;
    REQUIRE(client.request("POST", "/x", cat("cuerpo", i), "text/plain", r, err));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, cat("eco:/x:cuerpo", i));
  }
  // Pipelining: la respuesta diferida bloquea a las siguientes, que salen en orden.
  const std::string out = ls.raw(
      "GET /a HTTP/1.1\r\n\r\nGET /lento HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\nConnection: close\r\n\r\n", 500);
  const size_t a = out.find("eco:/a"), l = out.find("lento"), b = out.find("eco:/b");
  CHECK(a != std::string::npos && l != std::string::npos && b != std::string::npos);
  CHECK(a < l && l < b);
  CHECK_EQ(count_of(out, "HTTP/1.1 200"), size_t{3});
}

TEST(servidor_errores_continue_y_sse) {
  LiveServer ls([](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    if (req.path == "/sse") {
      s.start_sse(h);
      s.sse_send(h.id, "saludo", "{\"a\":1}");
      s.sse_send(h.id, "", "linea1\nlinea2");
      return;
    }
    if (req.path == "/olvido") return;  // no responde: el servidor contesta 500
    s.respond(h, HttpResponse::text(200, cat("bytes=", req.body.size())));
  });
  // Petición mal formada: 400 y cierre.
  std::string out = ls.raw("TONTERIA\r\n\r\n");
  CHECK(out.starts_with("HTTP/1.1 400"));
  CHECK(out.find("Connection: close") != std::string::npos);
  // Expect: 100-continue antes del cuerpo.
  out = ls.raw("POST /up HTTP/1.1\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n", 150);
  CHECK(out.starts_with("HTTP/1.1 100 Continue"));
  // Un manejador que no responde no deja la conexión colgada.
  out = ls.raw("GET /olvido HTTP/1.1\r\nConnection: close\r\n\r\n");
  CHECK(out.starts_with("HTTP/1.1 500"));
  // SSE: cabeceras sin longitud y eventos con varias líneas de datos.
  out = ls.raw("GET /sse HTTP/1.1\r\n\r\n", 200);
  CHECK(out.find("Content-Type: text/event-stream") != std::string::npos);
  CHECK(out.find("Content-Length") == std::string::npos);
  CHECK(out.find("event: saludo\ndata: {\"a\":1}\n\n") != std::string::npos);
  CHECK(out.find("data: linea1\ndata: linea2\n\n") != std::string::npos);
  // Cuerpo chunked real.
  out = ls.raw("POST /c HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
  CHECK(out.find("bytes=5") != std::string::npos);
}

TEST(servidor_post_desde_otros_hilos_no_pierde_avisos) {
  // Regresión: con una bandera de "aviso pendiente" separada del contador del
  // eventfd, un wake() que caía entre bajar la bandera y leer el eventfd dejaba
  // la bandera a true para siempre: desde entonces cada tarea esperaba al plazo
  // del poll (500 ms). Varios hilos encolan miles de tareas para provocar la
  // carrera y después se mide la latencia de ida y vuelta de unas cuantas.
  LiveServer ls([](HttpServer& s, const ConnHandle& h, HttpRequest&) { s.respond(h, HttpResponse::text(200, "ok")); });
  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;
  int executed = 0;  // solo lo tocan las tareas, en el hilo del servidor
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        ls.server->post([&executed] { ++executed; });
        if ((i + t) % 64 == 0) std::this_thread::yield();
      }
    });
  }
  for (auto& p : producers) p.join();

  // Ida y vuelta: encolar y esperar a que corra. Sin avisos perdidos tarda
  // microsegundos; con la bandera atascada, ~500 ms cada vez.
  int64_t worst_us = 0;
  int done = 0;
  for (int round = 0; round < 5; ++round) {
    std::atomic<bool> seen{false};
    const int64_t t0 = mono_us();
    ls.server->post([&] {
      done = executed;
      seen = true;
    });
    while (!seen && mono_us() - t0 < 2000000) std::this_thread::sleep_for(std::chrono::microseconds(100));
    REQUIRE(seen);
    worst_us = std::max(worst_us, mono_us() - t0);
  }
  CHECK_EQ(done, kThreads * kPerThread);
  CHECK(worst_us < 200000);
}
