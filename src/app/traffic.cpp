#include "app/traffic.hpp"

#include <algorithm>
#include <cmath>

#include "core/strings.hpp"

namespace zr::app {

namespace {

constexpr const char* kServices[] = {"gateway", "auth", "catalog", "checkout", "payments", "notifications"};
constexpr int64_t kIncidentPeriodMs = 150000;
constexpr int64_t kIncidentStartMs = 60000;
constexpr int64_t kIncidentLengthMs = 45000;

struct Weighted {
  const char* value;
  double weight;
};
constexpr Weighted kRegions[] = {{"sa-east-1", 0.42}, {"us-east-1", 0.33}, {"eu-west-1", 0.17}, {"ap-south-1", 0.08}};
constexpr Weighted kPlatforms[] = {{"android", 0.48}, {"web", 0.34}, {"ios", 0.18}};
constexpr const char* kVersions[] = {"5.2.1", "5.2.0", "5.1.4", "4.9.0"};

template <size_t N>
const char* pick(const Weighted (&items)[N], double u) {
  double acc = 0;
  for (const auto& it : items) {
    acc += it.weight;
    if (u < acc) return it.value;
  }
  return items[N - 1].value;
}

}  // namespace

TrafficGenerator::TrafficGenerator(uint64_t seed) : rng_(seed) {
  routes_ = {
      {"gateway", "GET", "/", 6, 12, 0.35, 0.002},
      {"gateway", "GET", "/health", 3, 2, 0.25, 0.0},
      {"auth", "POST", "/login", 3, 95, 0.45, 0.01},
      {"auth", "POST", "/token/refresh", 5, 28, 0.4, 0.004},
      {"catalog", "GET", "/products", 14, 45, 0.5, 0.003},
      {"catalog", "GET", "/products/{id}", 22, 22, 0.45, 0.004},
      {"catalog", "GET", "/search", 12, 120, 0.6, 0.006},
      {"checkout", "GET", "/cart", 9, 35, 0.4, 0.003},
      {"checkout", "POST", "/cart/items", 7, 60, 0.45, 0.008},
      {"checkout", "POST", "/checkout", 3, 240, 0.55, 0.015},
      {"payments", "POST", "/payments", 3, 310, 0.5, 0.012},
      {"payments", "GET", "/payments/{id}", 2, 40, 0.4, 0.004},
      {"notifications", "POST", "/notify", 4, 18, 0.5, 0.005},
  };
  double acc = 0;
  for (const auto& r : routes_) route_cdf_.push_back(acc += r.weight);
  // Zipf (s = 1.07) sobre 20 000 usuarios: unos pocos generan mucho tráfico.
  acc = 0;
  user_cdf_.reserve(20000);
  for (int i = 1; i <= 20000; ++i) user_cdf_.push_back(acc += 1.0 / std::pow(i, 1.07));
}

const TrafficGenerator::Route& TrafficGenerator::pick_route() {
  std::uniform_real_distribution<double> u(0, route_cdf_.back());
  const double x = u(rng_);
  const size_t i = static_cast<size_t>(std::lower_bound(route_cdf_.begin(), route_cdf_.end(), x) - route_cdf_.begin());
  return routes_[std::min(i, routes_.size() - 1)];
}

uint32_t TrafficGenerator::pick_user() {
  std::uniform_real_distribution<double> u(0, user_cdf_.back());
  const double x = u(rng_);
  return static_cast<uint32_t>(std::lower_bound(user_cdf_.begin(), user_cdf_.end(), x) - user_cdf_.begin()) + 1;
}

std::string TrafficGenerator::incident_at(int64_t now_ms) {
  const int64_t phase = ((now_ms % kIncidentPeriodMs) + kIncidentPeriodMs) % kIncidentPeriodMs;
  if (phase < kIncidentStartMs || phase >= kIncidentStartMs + kIncidentLengthMs) return {};
  // Rota entre los servicios con tráfico de negocio (no gateway).
  const int64_t cycle = now_ms / kIncidentPeriodMs;
  return kServices[1 + static_cast<size_t>(cycle % 5)];
}

void TrafficGenerator::generate(size_t n, int64_t now_ms, int64_t spread_ms, std::string& out) {
  std::uniform_real_distribution<double> unit(0, 1);
  std::normal_distribution<double> normal(0, 1);
  const std::string degraded = incident_at(now_ms);
  out.reserve(out.size() + n * 260);
  for (size_t i = 0; i < n; ++i) {
    const Route& r = pick_route();
    const bool incident = !degraded.empty() && degraded == r.service;
    double latency = r.median_ms * std::exp(r.sigma * normal(rng_));
    double error_rate = r.error_rate;
    if (incident) {
      latency *= 5.5;
      error_rate += 0.22;
    }
    // Cola larga ocasional (GC, reintentos...).
    if (unit(rng_) < 0.004) latency *= 8 + 10 * unit(rng_);
    int status = std::string_view(r.method) == "POST" ? 201 : 200;
    if (r.path == std::string_view("/health")) status = 200;
    const double e = unit(rng_);
    if (e < error_rate) {
      static constexpr int k5xx[] = {500, 502, 503, 504};
      status = k5xx[static_cast<size_t>(unit(rng_) * 4) % 4];
    } else if (e < error_rate + 0.025) {
      static constexpr int k4xx[] = {400, 401, 404, 404, 429};
      status = k4xx[static_cast<size_t>(unit(rng_) * 5) % 5];
    }
    const int64_t ts = now_ms - static_cast<int64_t>(unit(rng_) * static_cast<double>(spread_ms));
    const char* region = pick(kRegions, unit(rng_));
    const char* platform = pick(kPlatforms, unit(rng_));
    const char* version = kVersions[static_cast<size_t>(unit(rng_) * unit(rng_) * 4) % 4];
    std::string path = r.path;
    if (const size_t brace = path.find("{id}"); brace != std::string::npos)
      path.replace(brace, 4, std::to_string(1000 + static_cast<int>(unit(rng_) * 9000)));
    const double bytes = std::round(std::max(80.0, 900 * std::exp(0.8 * normal(rng_))));
    append_all(out, "{\"ts\":", ts, ",\"service\":\"", r.service, "\",\"method\":\"", r.method, "\",\"route\":\"",
               r.path, "\",\"path\":\"", path, "\",\"status\":", status, ",\"latency_ms\":",
               std::round(latency * 100) / 100, ",\"bytes\":", bytes, ",\"user\":\"u-", pick_user(), "\",\"region\":\"",
               region, "\",\"client\":{\"platform\":\"", platform, "\",\"version\":\"", version, "\"}}\n");
  }
}

}  // namespace zr::app
