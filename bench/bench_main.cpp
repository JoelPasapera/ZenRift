// Microbenchmarks de las piezas de la ruta caliente de ZenRift.
//
//   zenrift_bench            todos
//   zenrift_bench consulta   solo los que contienen "consulta" en el nombre
//
// Cada línea mide una pieza aislada en un solo hilo; el rendimiento de punta a
// punta (HTTP + log + consultas en varios hilos) se mide con `zenrift loadgen`.
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/traffic.hpp"
#include "core/arena.hpp"
#include "core/crc32c.hpp"
#include "core/flat_map.hpp"
#include "core/hash.hpp"
#include "core/time.hpp"
#include "engine/aggregate.hpp"
#include "engine/event_scanner.hpp"
#include "engine/field_layout.hpp"
#include "engine/partition_query.hpp"
#include "harness.hpp"
#include "json/value.hpp"
#include "sketch/ddsketch.hpp"
#include "sketch/hyperloglog.hpp"
#include "sketch/space_saving.hpp"
#include "storage/partition_log.hpp"
#include "zql/plan.hpp"
#include "zql/vm.hpp"

using namespace zr;
using zql::Value;

namespace {

constexpr size_t kEvents = 10000;

// Lote de eventos sintéticos ya escaneado, compartido por varios benchmarks.
struct Corpus {
  std::string body;  // NDJSON
  engine::FieldLayout layout;
  engine::EventScanner scanner;
  Arena arena;
  std::vector<engine::ScannedEvent> events;
  std::vector<Value> slots;
  int64_t now = 0;

  Corpus() {
    now = wall_ms();
    app::TrafficGenerator gen(42);
    gen.generate(kEvents, now, 5000, body);
    layout.build({"ts", "service", "route", "method", "status", "latency_ms", "user", "region"});
    engine::ScanReport report;
    scanner.scan_batch(body, layout, 0, now, arena, events, slots, report);
    if (report.rejected != 0 || events.size() != kEvents) {
      std::fprintf(stderr, "corpus inválido: %zu aceptados, %zu rechazados\n", events.size(), report.rejected);
      std::exit(1);
    }
  }

  const Value* event_slots(size_t i) const { return slots.data() + i * layout.size(); }
};

// Descarta lo que produce una consulta: aquí solo interesa el coste por evento.
struct NullSink final : engine::QuerySink {
  void pane_closed(uint64_t, uint32_t, std::unique_ptr<engine::Pane>) override {}
  void watermark(uint64_t, uint32_t, int64_t) override {}
  void early_panes(uint64_t, uint32_t, std::vector<std::unique_ptr<engine::Pane>>) override {}
};

using Bench = std::function<zrb::Result(Corpus&)>;

// ------------------------------------------------------------------ JSON

zrb::Result bench_scan(Corpus& c) {
  engine::EventScanner scanner;
  Arena arena;
  std::vector<engine::ScannedEvent> events;
  std::vector<Value> slots;
  auto r = zrb::measure("escaner.ndjson", kEvents, c.body.size(), [&] {
    arena.reset();
    events.clear();
    engine::ScanReport report;
    scanner.scan_batch(c.body, c.layout, 0, c.now, arena, events, slots, report);
    zrb::keep(events);
  });
  r.note = "valida y extrae 8 campos por evento (proyección)";
  return r;
}

zrb::Result bench_dom(Corpus& c) {
  std::vector<std::string_view> lines;
  for (size_t pos = 0; pos < c.body.size();) {
    size_t nl = c.body.find('\n', pos);
    if (nl == std::string::npos) nl = c.body.size();
    if (nl > pos) lines.emplace_back(c.body.data() + pos, nl - pos);
    pos = nl + 1;
  }
  auto r = zrb::measure("json.dom", lines.size(), c.body.size(), [&] {
    for (auto line : lines) {
      json::Value v;
      json::parse(line, v);
      zrb::keep(v);
    }
  });
  r.note = "árbol completo por evento, para comparar con el escáner";
  return r;
}

// ------------------------------------------------------------------ ZQL

zrb::Result bench_where(Corpus& c) {
  zql::FilterPlan filter;
  zql::Diagnostic err;
  if (!zql::compile_filter("(status >= 500 AND service IN ('payments', 'checkout')) OR latency_ms > 1000", filter,
                           err)) {
    std::fprintf(stderr, "filtro inválido: %s\n", err.to_string().c_str());
    std::exit(1);
  }
  std::vector<uint32_t> map;
  for (const auto& f : filter.fields) map.push_back(static_cast<uint32_t>(c.layout.slot_of(f)));
  Arena scratch;
  size_t matched = 0;
  auto r = zrb::measure("zql.where", kEvents, 0, [&] {
    matched = 0;
    for (size_t i = 0; i < kEvents; ++i) {
      zql::EvalContext ctx;
      ctx.fields = c.event_slots(i);
      ctx.field_map = map.data();
      ctx.scratch = &scratch;
      matched += zql::eval_true(filter.prog, ctx) ? 1 : 0;
    }
    zrb::keep(matched);
  });
  r.note = "VM de pila, lógica trivalente y conjunto IN con hash";
  return r;
}

zrb::Result run_query(Corpus& c, const char* name, std::string_view text, const char* note) {
  auto plan = std::make_shared<zql::QueryPlan>();
  zql::Diagnostic err;
  if (!zql::compile_query(text, zql::CompileOptions{}, *plan, err)) {
    std::fprintf(stderr, "consulta inválida: %s\n", err.to_string().c_str());
    std::exit(1);
  }
  auto layout = std::make_shared<engine::AggLayout>(plan);
  auto counters = std::make_shared<engine::QueryCounters>();
  NullSink sink;
  engine::PartitionQuery q(1, 0, layout, counters, 0, c.now, engine::RuntimeOptions{}, &sink);
  q.bind(c.layout);
  q.live = true;
  Arena scratch;
  auto r = zrb::measure(name, kEvents, 0, [&] {
    for (size_t i = 0; i < kEvents; ++i) {
      scratch.reset();
      q.on_event(engine::EventView{c.event_slots(i), c.events[i].ts, c.events[i].raw}, c.now, scratch);
    }
  });
  r.note = note;
  return r;
}

zrb::Result bench_query_service(Corpus& c) {
  return run_query(c, "consulta.por_servicio",
                   "SELECT service, count(*) AS n, p99(latency_ms) AS p99, avg(status >= 500) AS errores "
                   "FROM requests GROUP BY service WINDOW TUMBLING 10s",
                   "GROUP BY + count + DDSketch + avg(cond) por evento");
}

zrb::Result bench_query_users(Corpus& c) {
  return run_query(c, "consulta.distintos_topk",
                   "SELECT region, distinct(user) AS usuarios, topk(route, 5) AS rutas "
                   "FROM requests WHERE method = 'GET' GROUP BY region WINDOW HOPPING 1m EVERY 10s",
                   "WHERE + HyperLogLog + Space-Saving en ventana deslizante");
}

// ------------------------------------------------------------------ sketches

std::vector<double> lognormal_values(size_t n) {
  std::mt19937_64 rng(7);
  std::lognormal_distribution<double> dist(4.0, 0.8);
  std::vector<double> v(n);
  for (auto& x : v) x = dist(rng);
  return v;
}

zrb::Result bench_ddsketch(Corpus&) {
  const auto values = lognormal_values(1 << 16);
  auto r = zrb::measure("ddsketch.add", values.size(), 0, [&] {
    sketch::DDSketch s;
    for (double x : values) s.add(x);
    zrb::keep(s);
  });
  r.note = "incluye crear el sketch; error relativo 1 %";
  return r;
}

zrb::Result bench_ddsketch_merge(Corpus&) {
  const auto values = lognormal_values(1 << 16);
  std::vector<sketch::DDSketch> parts(16);
  for (size_t i = 0; i < values.size(); ++i) parts[i % parts.size()].add(values[i]);
  auto r = zrb::measure("ddsketch.merge", parts.size(), 0, [&] {
    sketch::DDSketch total;
    for (const auto& p : parts) total.merge(p);
    const double p99 = total.quantile(0.99);
    zrb::keep(p99);
  });
  r.note = "por panel fusionado (16 paneles y un p99 por tanda)";
  return r;
}

zrb::Result bench_hll(Corpus&) {
  constexpr size_t n = 1 << 16;
  auto r = zrb::measure("hyperloglog.add", n, 0, [&] {
    sketch::HyperLogLog h;
    for (uint64_t i = 0; i < n; ++i) h.add_hash(hash_u64(i));
    const double e = h.estimate();
    zrb::keep(e);
  });
  r.note = "p = 14, pasa de disperso a denso durante la tanda";
  return r;
}

zrb::Result bench_space_saving(Corpus&) {
  std::mt19937_64 rng(11);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<std::string> items(1 << 15);
  for (auto& s : items) s = "u-" + std::to_string(static_cast<int>(std::pow(u(rng), 3.0) * 20000));
  auto r = zrb::measure("space_saving.add", items.size(), 0, [&] {
    sketch::SpaceSaving s(256);
    for (const auto& it : items) s.add(it);
    zrb::keep(s);
  });
  r.note = "256 contadores, claves con sesgo tipo Zipf";
  return r;
}

// ------------------------------------------------------------------ núcleo

zrb::Result bench_crc_hw(Corpus&) {
  std::vector<uint8_t> buf(1 << 20);
  std::mt19937_64 rng(3);
  for (auto& b : buf) b = static_cast<uint8_t>(rng());
  auto r = zrb::measure("crc32c.1MiB", 1, buf.size(), [&] {
    const uint32_t c = crc32c(buf.data(), buf.size());
    zrb::keep(c);
  });
  r.note = crc32c_hw_available() ? "instrucción crc32 de SSE4.2" : "sin SSE4.2: slicing-by-8";
  return r;
}

zrb::Result bench_crc_sw(Corpus&) {
  std::vector<uint8_t> buf(1 << 20);
  std::mt19937_64 rng(3);
  for (auto& b : buf) b = static_cast<uint8_t>(rng());
  auto r = zrb::measure("crc32c.software.1MiB", 1, buf.size(), [&] {
    const uint32_t c = crc32c_sw(buf.data(), buf.size());
    zrb::keep(c);
  });
  r.note = "slicing-by-8 con tablas constexpr";
  return r;
}

zrb::Result bench_hash(Corpus&) {
  std::vector<uint8_t> buf(1 << 16);
  std::mt19937_64 rng(5);
  for (auto& b : buf) b = static_cast<uint8_t>(rng());
  constexpr size_t n = 4096;
  auto r = zrb::measure("hash.32B", n, n * 32, [&] {
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) acc ^= hash_bytes(buf.data() + (i * 13) % (buf.size() - 32), 32);
    zrb::keep(acc);
  });
  r.note = "multiplicación 64x64->128 (estilo wyhash)";
  return r;
}

constexpr size_t kMapKeys = 1 << 18;

std::vector<uint64_t> map_keys() {
  std::vector<uint64_t> keys(kMapKeys);
  for (size_t i = 0; i < keys.size(); ++i) keys[i] = hash_u64(i * 7919 + 1);
  return keys;
}

zrb::Result bench_flatmap(Corpus&) {
  const auto keys = map_keys();
  auto r = zrb::measure("flatmap.insert+find", kMapKeys * 2, 0, [&] {
    FlatMap<uint64_t, uint32_t> m;
    for (size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], static_cast<uint32_t>(i));
    uint64_t found = 0;
    for (uint64_t k : keys) found += *m.find(k);
    zrb::keep(found);
  });
  r.note = "Robin Hood, sin reservar: incluye los crecimientos";
  return r;
}

zrb::Result bench_unordered_map(Corpus&) {
  const auto keys = map_keys();
  auto r = zrb::measure("std::unordered_map", kMapKeys * 2, 0, [&] {
    std::unordered_map<uint64_t, uint32_t> m;
    for (size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], static_cast<uint32_t>(i));
    uint64_t found = 0;
    for (uint64_t k : keys) found += m.find(k)->second;
    zrb::keep(found);
  });
  r.note = "misma carga, como referencia";
  return r;
}

// ------------------------------------------------------------------ log

struct ScratchDir {
  std::filesystem::path path;
  ScratchDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() / ("zenrift-bench-" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

// El log se mide aparte: cada tanda escribe en un log nuevo y la mejor tanda
// cuenta. Las lecturas salen de la caché de páginas del sistema operativo.
std::vector<zrb::Result> bench_log(Corpus& c) {
  constexpr size_t rounds = 20;  // 20 x 10 000 eventos por tanda
  const size_t n = rounds * c.events.size();
  uint64_t payload_bytes = 0;
  for (const auto& e : c.events) payload_bytes += e.raw.size();
  payload_bytes *= rounds;

  double best_write = 1e300, best_read = 1e300;
  for (int round = 0; round < 3; ++round) {
    ScratchDir dir;
    storage::PartitionLog log;
    storage::LogOptions opts;
    opts.segment_bytes = 16u << 20;
    std::string err;
    if (!log.open(dir.path.string(), opts, err)) {
      std::fprintf(stderr, "log: %s\n", err.c_str());
      std::exit(1);
    }
    auto t0 = std::chrono::steady_clock::now();
    for (size_t r = 0; r < rounds; ++r) {
      for (const auto& e : c.events) {
        int64_t off;
        log.append(e.ts, e.raw, off, err);
      }
      log.flush(err);  // un flush por lote, como hace el shard
    }
    best_write = std::min(best_write, zrb::seconds_since(t0));

    storage::LogCursor cursor;
    std::string scratch;
    std::vector<storage::LogRecord> out;
    size_t seen = 0;
    t0 = std::chrono::steady_clock::now();
    for (;;) {
      out.clear();
      if (!log.read(cursor, 4096, 1u << 20, scratch, out, err) || out.empty()) break;
      seen += out.size();
    }
    best_read = std::min(best_read, zrb::seconds_since(t0));
    if (seen != n) {
      std::fprintf(stderr, "log: se leyeron %zu de %zu registros\n", seen, n);
      std::exit(1);
    }
  }
  zrb::Result w{"log.append", best_write, n, payload_bytes, "CRC32C por registro, flush por lote, sin fsync"};
  zrb::Result r{"log.read", best_read, n, payload_bytes, "verifica CRC al leer; desde la caché de páginas"};
  return {w, r};
}

}  // namespace

int main(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  std::printf("ZenRift %s — microbenchmarks (%u hilos de hardware, un hilo por benchmark)\n\n", ZENRIFT_VERSION,
              std::thread::hardware_concurrency());
  Corpus corpus;
  std::printf("Corpus: %zu eventos sintéticos, %.2f MB de NDJSON (%.0f bytes/evento)\n\n", corpus.events.size(),
              static_cast<double>(corpus.body.size()) / 1048576.0,
              static_cast<double>(corpus.body.size()) / static_cast<double>(corpus.events.size()));

  const std::vector<std::pair<const char*, Bench>> benches = {
      {"escaner.ndjson", bench_scan},
      {"json.dom", bench_dom},
      {"zql.where", bench_where},
      {"consulta.por_servicio", bench_query_service},
      {"consulta.distintos_topk", bench_query_users},
      {"ddsketch.add", bench_ddsketch},
      {"ddsketch.merge", bench_ddsketch_merge},
      {"hyperloglog.add", bench_hll},
      {"space_saving.add", bench_space_saving},
      {"crc32c.1MiB", bench_crc_hw},
      {"crc32c.software.1MiB", bench_crc_sw},
      {"hash.32B", bench_hash},
      {"flatmap.insert+find", bench_flatmap},
      {"std::unordered_map", bench_unordered_map},
  };
  zrb::print_header();
  for (const auto& [name, fn] : benches)
    if (filter.empty() || std::string_view(name).find(filter) != std::string_view::npos) zrb::print(fn(corpus));
  if (filter.empty() || std::string_view("log.append log.read").find(filter) != std::string_view::npos)
    for (const auto& r : bench_log(corpus)) zrb::print(r);
  return 0;
}
