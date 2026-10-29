// Pruebas del núcleo del motor sin hilos: escáner, agregaciones, ejecución por
// partición y finalización, con un reloj simulado.
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/arena.hpp"
#include "core/strings.hpp"
#include "engine/aggregate.hpp"
#include "engine/event_scanner.hpp"
#include "engine/field_layout.hpp"
#include "engine/finalizer.hpp"
#include "engine/partition_query.hpp"
#include "framework.hpp"
#include "json/value.hpp"
#include "zql/plan.hpp"
#include "zql/vm.hpp"
#include "core/hash.hpp"

using namespace zr;
using namespace zr::engine;
using zql::Value;

namespace {

// ------------------------------------------------------------------ escáner

struct ScanFixture {
  FieldLayout layout;
  EventScanner scanner;
  Arena arena;
  std::vector<ScannedEvent> events;
  std::vector<Value> slots;
  ScanReport report;

  explicit ScanFixture(std::vector<std::string> paths) { layout.build(std::move(paths)); }
  void scan(std::string_view body, int64_t ingest = 1761760000000) {
    events.clear();
    report = ScanReport();
    scanner.scan_batch(body, layout, layout.slot_of("ts"), ingest, arena, events, slots, report);
  }
  const Value& slot(size_t ev, std::string_view path) const {
    return slots[ev * layout.size() + static_cast<size_t>(layout.slot_of(path))];
  }
};

// ------------------------------------------------------------------ tubería síncrona

struct DirectSink : QuerySink {
  QueryFinalizer* fin = nullptr;
  FinalizerOutput out;
  int64_t now = 0;
  void pane_closed(uint64_t, uint32_t p, std::unique_ptr<Pane> pane) override { fin->on_pane(p, std::move(pane), out); }
  void watermark(uint64_t, uint32_t p, int64_t wm) override { fin->on_watermark(p, wm, out); }
  void early_panes(uint64_t, uint32_t p, std::vector<std::unique_ptr<Pane>> panes) override {
    fin->on_early(p, std::move(panes), now, out);
  }
};

struct Pipeline {
  std::shared_ptr<zql::QueryPlan> plan;
  std::shared_ptr<AggLayout> layout;
  FieldLayout fields;
  EventScanner scanner;
  std::unique_ptr<QueryFinalizer> fin;
  DirectSink sink;
  std::vector<std::unique_ptr<PartitionQuery>> parts;
  std::shared_ptr<QueryCounters> counters = std::make_shared<QueryCounters>();
  Arena arena;
  int64_t now = 0;

  Pipeline(std::string_view zql_text, uint32_t partitions, int64_t start_wall, RuntimeOptions ropts = {},
           FinalizerOptions fopts = {}) {
    plan = std::make_shared<zql::QueryPlan>();
    zql::Diagnostic err;
    if (!zql::compile_query(zql_text, zql::CompileOptions{}, *plan, err))
      zt::report_failure(__FILE__, __LINE__, "no compila: " + err.to_string());
    layout = std::make_shared<AggLayout>(plan);
    std::vector<std::string> paths{"ts"};
    for (const auto& f : plan->fields)
      if (f != "ts") paths.push_back(f);
    fields.build(paths);
    fin = std::make_unique<QueryFinalizer>("q", layout, partitions, fopts);
    sink.fin = fin.get();
    now = start_wall;
    for (uint32_t p = 0; p < partitions; ++p) {
      parts.push_back(std::make_unique<PartitionQuery>(1, p, layout, counters, 0, now, ropts, &sink));
      parts.back()->bind(fields);
      parts.back()->live = true;
    }
  }

  void feed(uint32_t partition, std::string_view body) {
    std::vector<ScannedEvent> evs;
    std::vector<Value> slots;
    ScanReport rep;
    scanner.scan_batch(body, fields, 0, now, arena, evs, slots, rep);
    if (rep.rejected) zt::report_failure(__FILE__, __LINE__, "eventos rechazados: " + rep.errors[0]);
    for (size_t i = 0; i < evs.size(); ++i) {
      EventView ev{slots.data() + i * fields.size(), evs[i].ts, evs[i].raw};
      parts[partition]->on_event(ev, now, arena);
    }
    sink.now = now;
    parts[partition]->advance(now);
  }

  void tick(int64_t to) {
    now = to;
    sink.now = now;
    for (auto& p : parts) p->advance(now);
  }

  // Resultados finales (no parciales) analizados.
  std::vector<json::Value> finals() const {
    std::vector<json::Value> r;
    for (const auto& w : sink.out.windows) {
      if (w.partial) continue;
      json::Value v;
      if (!json::parse(*w.json, v)) zt::report_failure(__FILE__, __LINE__, "JSON de resultado inválido");
      r.push_back(std::move(v));
    }
    return r;
  }
  std::vector<json::Value> partials() const {
    std::vector<json::Value> r;
    for (const auto& w : sink.out.windows) {
      if (!w.partial) continue;
      json::Value v;
      json::parse(*w.json, v);
      r.push_back(std::move(v));
    }
    return r;
  }
};

std::string ev(int64_t ts, std::string_view service, double latency, int status, std::string_view user = "u1") {
  return cat("{\"ts\":", ts, ",\"service\":\"", service, "\",\"latency_ms\":", latency, ",\"status\":", status,
             ",\"user\":\"", user, "\"}\n");
}

const json::Value& row(const json::Value& result, size_t i) { return result.get("rows")->items()[i]; }
double num(const json::Value& r, size_t col) { return r.items()[col].as_number(); }
std::string str(const json::Value& r, size_t col) { return r.items()[col].as_string(); }

constexpr int64_t T0 = 1761760000000;  // 2025-10-29T17:46:40Z, múltiplo de 10 s

}  // namespace

// ============================================================================

TEST(escaner_ndjson_con_lineas_rotas_y_campos_anidados) {
  ScanFixture f({"ts", "service", "http.status", "tags.1", "client", "msg"});
  f.scan("{\"ts\":1761760000123,\"service\":\"api\",\"http\":{\"status\":503,\"x\":[1,2]},\"tags\":[\"a\",\"b\"]}\n"
         "{\"ts\":\"2025-10-29T13:45:00Z\",\"service\":\"pay\\u00e9\",\"client\":{\"os\":\"android\"}}\n"
         "{\"roto\": tru}\n"
         "no-json\n"
         "  {\"service\":\"sin-ts\",\"msg\":\"l\\nnea\"}  ");
  CHECK_EQ(f.report.accepted, size_t{3});
  CHECK_EQ(f.report.rejected, size_t{2});
  REQUIRE(f.events.size() == 3);
  CHECK_EQ(f.events[0].ts, int64_t{1761760000123});
  CHECK_EQ(f.slot(0, "http.status").num, 503.0);
  CHECK_EQ(f.slot(0, "tags.1").str(), std::string_view("b"));
  CHECK_EQ(f.slot(1, "service").str(), std::string_view("pay\xc3\xa9"));
  CHECK_EQ(f.slot(1, "client").str(), std::string_view("{\"os\":\"android\"}"));
  CHECK(f.slot(1, "client").type == zql::VType::Json);
  CHECK(!f.events[1].ts_defaulted);
  CHECK(f.events[2].ts_defaulted);
  CHECK_EQ(f.events[2].ts, int64_t{1761760000000});
  CHECK_EQ(f.slot(2, "msg").str(), std::string_view("l\nnea"));
  CHECK(f.report.errors[0].find("evento 3 ") != std::string::npos);  // numeración desde 1
}

TEST(escaner_array_json_normaliza_saltos_y_valida_tiempos) {
  ScanFixture f({"ts", "v"});
  f.scan("[\n  {\n    \"ts\": 1761760000,\n    \"v\": 1\n  },\n  {\"ts\": 9999999999999, \"v\": 2},\n"
         "  {\"ts\": \"ayer\", \"v\": 3},\n  42,\n  {\"v\": 4}\n]");
  CHECK_EQ(f.report.accepted, size_t{2});
  CHECK_EQ(f.report.rejected, size_t{3});
  REQUIRE(f.events.size() == 2);
  CHECK_EQ(f.events[0].ts, int64_t{1761760000000});  // segundos -> ms
  CHECK(f.events[0].raw.find('\n') == std::string_view::npos);
  CHECK_EQ(f.slot(1, "v").num, 4.0);
  // Un array roto descarta el resto pero conserva lo anterior.
  f.scan("[{\"v\":1},{\"v\":2},{\"v\":");
  CHECK_EQ(f.report.accepted, size_t{2});
  CHECK_EQ(f.report.rejected, size_t{1});
}

TEST(escaner_salta_subarboles_no_pedidos) {
  ScanFixture f({"ts", "a.b"});
  std::string big = "{\"ts\":1,\"x\":{\"y\":[";
  for (int i = 0; i < 1000; ++i) big += "{\"z\":\"" + std::to_string(i) + "\"},";
  big += "null]},\"a\":{\"b\":true,\"c\":[1,2,3]}}";
  f.scan(big);
  REQUIRE(f.events.size() == 1);
  CHECK(f.slot(0, "a.b").type == zql::VType::Bool);
  // Y relee un evento guardado con otra disposición de campos.
  FieldLayout other;
  other.build({"x.y.999.z"});
  Value slot;
  REQUIRE(f.scanner.scan_stored(f.events[0].raw, other, f.arena, &slot));
  CHECK_EQ(slot.str(), std::string_view("999"));
}

TEST(agregaciones_fusionadas_igualan_a_un_solo_panel) {
  auto plan = std::make_shared<zql::QueryPlan>();
  zql::Diagnostic err;
  REQUIRE(zql::compile_query(
      "SELECT service, count(*), count(user), sum(latency_ms), avg(latency_ms), min(latency_ms), max(latency_ms), "
      "stddev(latency_ms), p50(latency_ms), p99(latency_ms), distinct(user), topk(user, 3), first(user), last(user), "
      "rate() FROM r GROUP BY service",
      zql::CompileOptions{}, *plan, err));
  auto layout = std::make_shared<AggLayout>(plan);
  FieldLayout fields;
  fields.build({"ts", "service", "user", "latency_ms"});
  EventScanner sc;
  Arena arena;
  std::mt19937 rng(9);
  std::string body;
  for (int i = 0; i < 3000; ++i)
    body += ev(T0 + i, i % 3 ? "api" : "pay", 1 + (rng() % 5000) / 10.0, 200, "u" + std::to_string(rng() % 40));
  std::vector<ScannedEvent> evs;
  std::vector<Value> slots;
  ScanReport rep;
  sc.scan_batch(body, fields, 0, T0, arena, evs, slots, rep);
  REQUIRE(evs.size() == 3000);

  auto feed = [&](Pane& pane, size_t i) {
    zql::EvalContext ctx;
    std::vector<uint32_t> map;
    for (const auto& f : plan->fields) map.push_back(static_cast<uint32_t>(fields.slot_of(f)));
    ctx.fields = slots.data() + i * fields.size();
    ctx.field_map = map.data();
    ctx.scratch = &arena;
    std::string key;
    zql::encode_value(zql::eval(plan->keys[0], ctx), key);
    char* block = pane.group(key, hash_bytes(key.data(), key.size()), 1000);
    for (const auto& e : layout->entries()) {
      const Value v = e.spec->has_arg ? zql::eval(e.spec->arg, ctx) : Value::null();
      e.ops->update(block + e.offset, v, evs[i].ts, *e.spec);
    }
    ++pane.events;
  };
  Pane whole(layout, T0, T0 + 10000), a(layout, T0, T0 + 10000), b(layout, T0, T0 + 10000);
  for (size_t i = 0; i < evs.size(); ++i) {
    feed(whole, i);
    feed(rng() % 2 ? a : b, i);
  }
  Pane merged(layout, T0, T0 + 10000);
  merged.merge_from(a, 1000);
  merged.merge_from(b, 1000);
  auto cloned = merged.clone();
  CHECK_EQ(merged.events, whole.events);
  CHECK_EQ(cloned->groups(), whole.groups());

  // Comparar resultados agregado a agregado.
  Arena scratch;
  ResultContext rctx{10000, &scratch};
  whole.for_each([&](std::string_view key, const char* wb) {
    const char* mb = nullptr;
    cloned->for_each([&](std::string_view k2, const char* blk) {
      if (k2 == key) mb = blk;
    });
    REQUIRE(mb != nullptr);
    for (const auto& e : layout->entries()) {
      const Value x = e.ops->result(wb + e.offset, *e.spec, rctx);
      const Value y = e.ops->result(mb + e.offset, *e.spec, rctx);
      std::string sx, sy;
      zql::write_json(sx, x);
      zql::write_json(sy, y);
      if (e.spec->kind == zql::AggKind::Sum || e.spec->kind == zql::AggKind::Avg ||
          e.spec->kind == zql::AggKind::Stddev) {
        CHECK_NEAR(x.num, y.num, 1e-9 * std::fabs(x.num) + 1e-9);
      } else if (sx != sy) {
        zt::report_failure(__FILE__, __LINE__, cat(e.spec->signature, ": ", sx, " vs ", sy));
      }
    }
  });
}

TEST(tuberia_tumbling_cierra_ventanas_y_cuenta_tardios) {
  Pipeline pl("SELECT service, count(*) AS n, max(latency_ms) AS mx FROM r GROUP BY service WINDOW TUMBLING 10s "
              "LATENESS 1s",
              1, T0);
  std::string b;
  for (int i = 0; i < 25; ++i) b += ev(T0 + i * 1000, i % 2 ? "api" : "pay", 10 + i, 200);
  pl.feed(0, b);  // max_ts = T0+24s -> marca = T0+23s: cierra [T0, T0+10s) y [T0+10s, T0+20s)
  auto res = pl.finals();
  REQUIRE(res.size() == 2);
  CHECK_EQ(res[0].get_number("start_ms", 0), static_cast<double>(T0));
  REQUIRE(res[0].get("rows")->items().size() == 2);
  CHECK_EQ(str(row(res[0], 0), 0), std::string("api"));  // ordenado por clave
  CHECK_EQ(num(row(res[0], 0), 1), 5.0);
  CHECK_EQ(num(row(res[0], 1), 1), 5.0);
  CHECK_EQ(num(row(res[1], 0), 2), 29.0);
  // Evento tardío para la primera ventana.
  pl.feed(0, ev(T0 + 5000, "api", 1, 200));
  CHECK_EQ(pl.counters->late.load(), uint64_t{1});
  // Sin más eventos, la inactividad cierra la tercera ventana.
  pl.tick(pl.now + 200);
  CHECK_EQ(pl.finals().size(), size_t{2});
  pl.tick(pl.now + 10000);
  res = pl.finals();
  REQUIRE(res.size() == 3);
  CHECK_EQ(res[2].get_number("events", 0), 5.0);
  CHECK(pl.fin->history(10).size() == 3);
}

TEST(tuberia_global_emite_ventanas_vacias_y_hopping_suma_paneles) {
  Pipeline g("SELECT count(*) AS n, sum(latency_ms) AS s FROM r WINDOW TUMBLING 1s LATENESS 0s", 1, T0);
  g.feed(0, ev(T0 + 100, "a", 5, 200));
  g.feed(0, ev(T0 + 3500, "a", 7, 200));  // las ventanas 1 y 2 quedan vacías
  auto res = g.finals();
  REQUIRE(res.size() == 3);
  CHECK_EQ(num(row(res[0], 0), 0), 1.0);
  CHECK_EQ(num(row(res[1], 0), 0), 0.0);
  CHECK(row(res[1], 0).items()[1].is_null());  // sum de nada = null
  CHECK_EQ(num(row(res[2], 0), 0), 0.0);

  Pipeline h("SELECT count(*) AS n FROM r WINDOW HOPPING 3s EVERY 1s LATENESS 0s", 1, T0);
  std::string b;
  for (int s = 0; s < 6; ++s)
    for (int k = 0; k <= s; ++k) b += ev(T0 + s * 1000 + k, "a", 1, 200);  // 1, 2, 3, 4, 5, 6 eventos por segundo
  h.feed(0, b);
  res = h.finals();
  // Ventanas que terminan en T0+1s..T0+5s: 1, 1+2, 1+2+3, 2+3+4, 3+4+5.
  REQUIRE(res.size() == 5);
  const double expected[] = {1, 3, 6, 9, 12};
  for (size_t i = 0; i < 5; ++i) CHECK_EQ(num(row(res[i], 0), 0), expected[i]);
  CHECK_EQ(res[4].get_number("end_ms", 0) - res[4].get_number("start_ms", 0), 3000.0);
}

TEST(tuberia_varias_particiones_coincide_con_una) {
  const char* q = "SELECT service, count(*) AS n, p99(latency_ms) AS p99, distinct(user) AS u FROM r "
                  "GROUP BY service WINDOW TUMBLING 5s LATENESS 0s";
  Pipeline one(q, 1, T0), three(q, 3, T0);
  std::mt19937 rng(11);
  std::vector<std::string> bodies(3);
  std::string all;
  for (int i = 0; i < 6000; ++i) {
    const std::string e = ev(T0 + i * 3, i % 4 ? "api" : "pay", (rng() % 10000) / 10.0, 200,
                             "u" + std::to_string(rng() % 500));
    all += e;
    bodies[rng() % 3] += e;
  }
  one.feed(0, all);
  for (uint32_t p = 0; p < 3; ++p) three.feed(p, bodies[p]);
  one.tick(T0 + 60000);
  three.tick(T0 + 60000);
  const auto a = one.finals(), b = three.finals();
  REQUIRE(a.size() == b.size());
  REQUIRE(!a.empty());
  for (size_t i = 0; i < a.size(); ++i) CHECK_EQ(json::to_string(*a[i].get("rows")), json::to_string(*b[i].get("rows")));
}

TEST(tuberia_having_order_limit_y_parciales) {
  Pipeline pl("SELECT service, count(*) AS n FROM r GROUP BY service WINDOW TUMBLING 10s LATENESS 0s "
              "HAVING n >= 2 ORDER BY n DESC LIMIT 2 EMIT EVERY 1s",
              1, T0);
  std::string b;
  const char* names[] = {"a", "b", "b", "c", "c", "c", "d", "d", "d", "d"};
  for (int i = 0; i < 10; ++i) b += ev(T0 + i * 100, names[i], 1, 200);
  pl.feed(0, b);
  pl.tick(T0 + 1500);  // parcial de la ventana en curso
  const auto parts = pl.partials();
  REQUIRE(!parts.empty());
  CHECK(parts.back().get_bool("partial", false));
  REQUIRE(parts.back().get("rows")->items().size() == 2);
  CHECK_EQ(str(row(parts.back(), 0), 0), std::string("d"));
  pl.feed(0, ev(T0 + 10500, "z", 1, 200));
  const auto res = pl.finals();
  REQUIRE(res.size() == 1);
  CHECK(res[0].get_bool("truncated", false));  // c y b también pasaban el HAVING
  REQUIRE(res[0].get("rows")->items().size() == 2);
  CHECK_EQ(str(row(res[0], 0), 0), std::string("d"));
  CHECK_EQ(str(row(res[0], 1), 0), std::string("c"));
  CHECK_EQ(res[0].get_number("groups", 0), 4.0);
}

TEST(tuberia_alertas_con_histeresis) {
  Pipeline pl("SELECT service, avg(status >= 500) * 100 AS err FROM r GROUP BY service WINDOW TUMBLING 1s "
              "LATENESS 0s ALERT IF err > 50 FOR 2 WINDOWS",
              1, T0);
  auto second = [&](int s, int errors, int ok) {
    std::string b;
    for (int i = 0; i < errors; ++i) b += ev(T0 + s * 1000 + i, "pay", 1, 503);
    for (int i = 0; i < ok; ++i) b += ev(T0 + s * 1000 + 500 + i, "pay", 1, 200);
    pl.feed(0, b);
  };
  second(0, 9, 1);  // 90 % de errores: racha 1
  second(1, 9, 1);  // racha 2 al cerrar esta ventana -> firing
  second(2, 1, 9);  // 10 %: resuelve
  second(3, 0, 1);
  std::vector<std::string> states;
  for (const auto& t : pl.sink.out.alerts) {
    json::Value v;
    REQUIRE(json::parse(*t.json, v));
    states.push_back(cat(v.get_string("state"), "@", (v.get_number("at_ms", 0) - T0) / 1000));
    CHECK_EQ(v.get("group")->get_string("service"), std::string_view("pay"));
  }
  REQUIRE(states.size() == 2);
  CHECK_EQ(states[0], std::string("firing@2"));
  CHECK_EQ(states[1], std::string("resolved@3"));
}
