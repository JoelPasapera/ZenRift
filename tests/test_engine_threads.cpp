// Pruebas del motor completo con sus hilos (shards + coordinador) y disco real.
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/strings.hpp"
#include "core/time.hpp"
#include "engine/engine.hpp"
#include "framework.hpp"
#include "json/value.hpp"
#include "temp_dir.hpp"

using namespace zr;
using namespace zr::engine;

namespace {

class Collector : public EngineListener {
 public:
  void reply(const ReplyTo& to, Response r) override {
    std::lock_guard<std::mutex> lock(m_);
    replies_[to.request] = std::move(r);
    cv_.notify_all();
  }
  void publish(Publication p) override {
    std::lock_guard<std::mutex> lock(m_);
    pubs_.push_back(std::move(p));
    cv_.notify_all();
  }

  Response wait_reply(uint64_t id, int timeout_ms = 5000) {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return replies_.count(id) > 0; });
    auto it = replies_.find(id);
    if (it == replies_.end()) return Response{0, "", "(sin respuesta)"};
    Response r = it->second;
    replies_.erase(it);
    return r;
  }

  // Espera a que haya `n` publicaciones que cumplan `pred`.
  template <class P>
  std::vector<Publication> wait_pubs(P pred, size_t n, int timeout_ms = 8000) {
    std::unique_lock<std::mutex> lock(m_);
    std::vector<Publication> found;
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
      found.clear();
      for (const auto& p : pubs_)
        if (pred(p)) found.push_back(p);
      return found.size() >= n;
    });
    return found;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::map<uint64_t, Response> replies_;
  std::vector<Publication> pubs_;
};

struct Rig {
  zt::TempDir dir;
  Collector listener;
  std::unique_ptr<Engine> engine;
  uint64_t next_id = 1;

  EngineConfig config() const {
    EngineConfig c;
    c.data_dir = dir.str();
    c.shards = 2;
    c.idle_ms = 200;
    c.fsync = FsyncPolicy::Never;
    return c;
  }

  void start() {
    engine = std::make_unique<Engine>(config(), &listener);
    std::string err;
    if (!engine->start(err)) zt::report_failure(__FILE__, __LINE__, "no arrancó: " + err);
  }
  void stop() {
    if (engine) engine->stop();
    engine.reset();
  }
  ReplyTo to() { return ReplyTo{1, next_id++}; }

  Response call(const std::function<void(ReplyTo)>& f) {
    const ReplyTo r = to();
    f(r);
    return listener.wait_reply(r.request);
  }
};

std::string events_body(int64_t ts0, int n, int step_ms, const char* service) {
  std::string b;
  for (int i = 0; i < n; ++i)
    b += cat("{\"ts\":", ts0 + i * step_ms, ",\"service\":\"", service, "\",\"latency_ms\":", 10 + i % 7,
             ",\"status\":", i % 10 == 0 ? 500 : 200, "}\n");
  return b;
}

double total_count(const std::vector<Publication>& results, size_t col) {
  double t = 0;
  for (const auto& p : results) {
    json::Value v;
    if (!json::parse(*p.data, v)) continue;
    for (const auto& row : v.get("rows")->items()) t += row.items()[col].as_number();
  }
  return t;
}

}  // namespace

TEST(motor_hilos_ingesta_consulta_y_resultados) {
  Rig rig;
  rig.start();
  StreamConfig sc;
  sc.name = "requests";
  sc.partitions = 3;
  Response r = rig.call([&](ReplyTo t) { rig.engine->create_stream(sc, t); });
  CHECK_EQ(r.status, 201);
  r = rig.call([&](ReplyTo t) {
    rig.engine->create_query("por_servicio",
                             "SELECT service, count(*) AS n, avg(status >= 500) AS err FROM requests "
                             "GROUP BY service WINDOW TUMBLING 1s LATENESS 0s",
                             "now", t);
  });
  REQUIRE(r.status == 201);
  // Eventos con tiempo de pared actual, repartidos en varios lotes y particiones.
  const int64_t t0 = align_down(wall_ms(), 1000);
  int sent = 0;
  for (int b = 0; b < 6; ++b) {
    const std::string body = events_body(t0 + b * 100, 50, 1, b % 2 ? "api" : "pay");
    r = rig.call([&](ReplyTo t) { rig.engine->ingest("requests", body, t); });
    CHECK_EQ(r.status, 200);
    json::Value v;
    REQUIRE(json::parse(r.body, v));
    CHECK_EQ(v.get_number("accepted", 0), 50.0);
    sent += 50;
  }
  // La inactividad cierra la ventana: resultados finales con todos los eventos.
  auto is_result = [](const Publication& p) { return p.topic == Topic::QueryResult && p.key == "por_servicio"; };
  const auto results = rig.listener.wait_pubs(
      [&](const Publication& p) { return is_result(p) && p.data->find("\"rows\":[[") != std::string::npos; }, 1);
  REQUIRE(!results.empty());
  CHECK_EQ(total_count(results, 1), static_cast<double>(sent));

  // Lectura del log, consulta y listado.
  r = rig.call([&](ReplyTo t) { rig.engine->get_query("por_servicio", t); });
  CHECK_EQ(r.status, 200);
  CHECK(r.body.find("\"explain\"") != std::string::npos);
  bool found = false;
  const std::string s = rig.engine->stream_json("requests", found);
  CHECK(found);
  CHECK(s.find("\"events\":300") != std::string::npos);
  r = rig.call([&](ReplyTo t) { CHECK(rig.engine->read_events("requests", 0, 0, 5, t)); });
  CHECK_EQ(r.status, 200);
  json::Value page;
  REQUIRE(json::parse(r.body, page));
  CHECK_EQ(page.get("events")->items().size(), size_t{5});
  rig.stop();
}

TEST(motor_hilos_errores_de_api) {
  Rig rig;
  rig.start();
  Response r = rig.call([&](ReplyTo t) { rig.engine->create_query("mala", "SELECT count(*)\nFROM r WHERE", "now", t); });
  CHECK_EQ(r.status, 400);
  json::Value v;
  REQUIRE(json::parse(r.body, v));
  CHECK_EQ(v.get_number("line", 0), 2.0);
  r = rig.call([&](ReplyTo t) { rig.engine->create_query("ok", "SELECT count(*) FROM s1", "now", t); });
  CHECK_EQ(r.status, 201);  // crea el stream s1 automáticamente
  r = rig.call([&](ReplyTo t) { rig.engine->create_query("ok", "SELECT count(*) FROM s1", "now", t); });
  CHECK_EQ(r.status, 409);
  r = rig.call([&](ReplyTo t) { rig.engine->create_query("x", "SELECT count(*) FROM s1", "mañana", t); });
  CHECK_EQ(r.status, 400);
  r = rig.call([&](ReplyTo t) { rig.engine->delete_stream("s1", false, t); });
  CHECK_EQ(r.status, 409);
  r = rig.call([&](ReplyTo t) { rig.engine->delete_stream("s1", true, t); });
  CHECK_EQ(r.status, 200);
  r = rig.call([&](ReplyTo t) { rig.engine->get_query("ok", t); });
  CHECK_EQ(r.status, 404);
  r = rig.call([&](ReplyTo t) { rig.engine->ingest("nuevo", "{\"a\":1}\n{\"a\":", t); });
  CHECK_EQ(r.status, 200);  // creado al vuelo; 1 aceptado y 1 rechazado
  REQUIRE(json::parse(r.body, v));
  CHECK_EQ(v.get_number("accepted", 0), 1.0);
  CHECK_EQ(v.get_number("rejected", 0), 1.0);
  rig.stop();
}

TEST(motor_hilos_reinicio_recupera_catalogo_log_y_backfill) {
  Rig rig;
  rig.start();
  StreamConfig sc;
  sc.name = "logs";
  sc.partitions = 2;
  CHECK_EQ(rig.call([&](ReplyTo t) { rig.engine->create_stream(sc, t); }).status, 201);
  const int64_t t0 = align_down(wall_ms() - 120000, 1000);  // hace dos minutos
  for (int b = 0; b < 4; ++b) {
    const std::string body = events_body(t0 + b * 1000, 100, 5, "svc");
    CHECK_EQ(rig.call([&](ReplyTo t) { rig.engine->ingest("logs", body, t); }).status, 200);
  }
  rig.stop();

  // Segundo arranque sobre el mismo directorio.
  rig.start();
  bool found = false;
  std::string s;
  for (int i = 0; i < 100; ++i) {  // las particiones se abren de forma asíncrona
    s = rig.engine->stream_json("logs", found);
    if (s.find("\"end_offset\":200") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(found);
  CHECK(s.find("\"end_offset\":200") != std::string::npos);  // 400 eventos en 2 particiones
  // Consulta con backfill desde el principio: debe ver los 400 eventos del log.
  Response r = rig.call([&](ReplyTo t) {
    rig.engine->create_query("total", "SELECT count(*) AS n FROM logs WINDOW TUMBLING 1m LATENESS 0s", "beginning", t);
  });
  REQUIRE(r.status == 201);
  // Los 400 eventos pueden caer en una o dos ventanas de 1 minuto: esperar
  // hasta que la suma de todas las ventanas publicadas llegue a 400.
  double total = 0;
  for (int i = 0; i < 80 && total < 400; ++i) {
    const auto results = rig.listener.wait_pubs(
        [](const Publication& p) { return p.topic == Topic::QueryResult && p.key == "total"; }, 1, 100);
    total = total_count(results, 0);
    if (total < 400) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK_EQ(total, 400.0);
  rig.stop();

  // Tercer arranque: la consulta se restaura sola desde el catálogo.
  rig.start();
  r = rig.call([&](ReplyTo t) { rig.engine->get_query("total", t); });
  CHECK_EQ(r.status, 200);
  rig.stop();
}

TEST(motor_hilos_tail_filtrado) {
  Rig rig;
  rig.start();
  CHECK_EQ(rig.call([&](ReplyTo t) { rig.engine->ingest("t", "{\"a\":0}", t); }).status, 200);
  zql::FilterPlan* fp = new zql::FilterPlan();
  zql::Diagnostic err;
  REQUIRE(zql::compile_filter("a >= 5 AND kind = 'x'", *fp, err));
  REQUIRE(rig.engine->add_tail(77, "t", std::shared_ptr<const zql::FilterPlan>(fp)));
  CHECK(!rig.engine->add_tail(78, "no-existe", nullptr));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::string body;
  for (int i = 0; i < 10; ++i) body += cat("{\"a\":", i, ",\"kind\":\"", i % 2 ? "x" : "y", "\"}\n");
  CHECK_EQ(rig.call([&](ReplyTo t) { rig.engine->ingest("t", body, t); }).status, 200);
  const auto pubs = rig.listener.wait_pubs([](const Publication& p) { return p.topic == Topic::Tail && p.key == "77"; }, 1);
  REQUIRE(pubs.size() == 1);
  CHECK_EQ(*pubs[0].data, std::string("[{\"a\":5,\"kind\":\"x\"},{\"a\":7,\"kind\":\"x\"},{\"a\":9,\"kind\":\"x\"}]"));
  rig.engine->remove_tail(77);
  rig.stop();
}
