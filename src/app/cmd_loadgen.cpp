// zenrift loadgen: genera tráfico sintético contra un servidor y mide el
// rendimiento de la ingesta (eventos/s y latencia de cada lote).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "app/commands.hpp"
#include "app/traffic.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "net/http_client.hpp"
#include "sketch/ddsketch.hpp"

namespace zr::app {

namespace {

struct LoadOptions {
  std::string url = "http://127.0.0.1:7070";
  std::string stream = "requests";
  std::string token;
  double rate = 0;  // 0 = tan rápido como se pueda
  int64_t duration_ms = 10000;
  uint32_t batch = 500;
  uint32_t connections = 4;
};

struct Shared {
  std::atomic<uint64_t> events{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> errors{0};
  std::mutex m;
  sketch::DDSketch latency;
  std::string last_error;
};

void worker(const LoadOptions& o, const std::string& host, uint16_t port, uint32_t id, Shared& sh,
            std::atomic<bool>& stop) {
  net::HttpClient client(host, port, o.token);
  TrafficGenerator gen(0x5eed0000ull + id);
  const std::string path = cat("/v1/streams/", o.stream, "/events");
  const double my_rate = o.rate > 0 ? o.rate / o.connections : 0;
  auto next = std::chrono::steady_clock::now();
  std::string body;
  sketch::DDSketch local;
  while (!stop.load(std::memory_order_relaxed)) {
    body.clear();
    gen.generate(o.batch, wall_ms(), 50, body);
    const int64_t t0 = mono_us();
    net::ClientResponse resp;
    std::string err;
    const bool ok = client.request("POST", path, body, "application/x-ndjson", resp, err);
    const int64_t dt = mono_us() - t0;
    if (!ok || resp.status != 200) {
      sh.errors.fetch_add(1);
      std::lock_guard<std::mutex> lock(sh.m);
      sh.last_error = ok ? cat("HTTP ", resp.status, ": ", resp.body.substr(0, 200)) : err;
      if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    sh.events.fetch_add(o.batch, std::memory_order_relaxed);
    sh.bytes.fetch_add(body.size(), std::memory_order_relaxed);
    local.add(static_cast<double>(dt) / 1000.0);
    if (my_rate > 0) {
      next += std::chrono::microseconds(static_cast<int64_t>(1e6 * o.batch / my_rate));
      std::this_thread::sleep_until(next);
    }
  }
  std::lock_guard<std::mutex> lock(sh.m);
  sh.latency.merge(local);
}

}  // namespace

int cmd_loadgen(const std::vector<std::string>& args) {
  LoadOptions o;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "error: falta el valor de %s\n", a.c_str());
        std::exit(2);
      }
      return args[++i];
    };
    int64_t n = 0;
    double d = 0;
    if (a == "--url") o.url = value();
    else if (a == "--stream") o.stream = value();
    else if (a == "--token") o.token = value();
    else if (a == "--rate" && parse_double(value(), d)) o.rate = d;
    else if (a == "--duration") {
      if (!parse_duration(value(), o.duration_ms)) {
        std::fprintf(stderr, "error: --duration espera algo como 30s\n");
        return 2;
      }
    } else if (a == "--batch" && parse_int64(value(), n) && n > 0) o.batch = static_cast<uint32_t>(n);
    else if (a == "--connections" && parse_int64(value(), n) && n > 0) o.connections = static_cast<uint32_t>(n);
    else {
      std::fprintf(stderr,
                   "uso: zenrift loadgen [--url http://127.0.0.1:7070] [--stream requests] [--rate eventos/s]\n"
                   "                     [--duration 10s] [--batch 500] [--connections 4] [--token T]\n");
      return 2;
    }
  }
  std::string err, host;
  uint16_t port = 0;
  if (!net::net_init(err) || !net::parse_http_url(o.url, host, port, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }
  std::printf("Enviando a %s/v1/streams/%s/events con %u conexión(es), lotes de %u eventos, %s durante %s\n",
              o.url.c_str(), o.stream.c_str(), o.connections, o.batch,
              o.rate > 0 ? cat(o.rate, " eventos/s").c_str() : "sin límite de ritmo",
              format_duration(o.duration_ms).c_str());
  Shared sh;
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < o.connections; ++i)
    threads.emplace_back(worker, std::cref(o), std::cref(host), port, i, std::ref(sh), std::ref(stop));
  const int64_t start = mono_ms();
  uint64_t last = 0;
  while (mono_ms() - start < o.duration_ms) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t now = sh.events.load();
    std::printf("  %6.1fs  %10.0f eventos/s  (total %llu, errores %llu)\n", (mono_ms() - start) / 1000.0,
                static_cast<double>(now - last), static_cast<unsigned long long>(now),
                static_cast<unsigned long long>(sh.errors.load()));
    std::fflush(stdout);
    last = now;
  }
  stop = true;
  for (auto& t : threads) t.join();
  const double secs = static_cast<double>(mono_ms() - start) / 1000.0;
  const uint64_t total = sh.events.load();
  std::printf("\nResumen: %llu eventos en %.1f s = %.0f eventos/s (%.1f MB/s de NDJSON)\n",
              static_cast<unsigned long long>(total), secs, static_cast<double>(total) / secs,
              static_cast<double>(sh.bytes.load()) / secs / 1048576.0);
  if (sh.latency.count() > 0)
    std::printf("Latencia por lote: p50 %.2f ms, p99 %.2f ms, máx %.2f ms\n", sh.latency.quantile(0.5),
                sh.latency.quantile(0.99), sh.latency.max());
  if (sh.errors.load() > 0)
    std::printf("Errores: %llu (último: %s)\n", static_cast<unsigned long long>(sh.errors.load()),
                sh.last_error.c_str());
  return sh.errors.load() > 0 && total == 0 ? 1 : 0;
}

}  // namespace zr::app
