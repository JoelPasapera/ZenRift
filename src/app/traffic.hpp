// Generador de tráfico sintético: peticiones HTTP de una tienda en línea con
// microservicios, usuarios con distribución de Zipf, latencias log-normales y
// un incidente periódico (cada 2,5 min un servicio se degrada durante 45 s),
// para que la demo muestre picos de latencia, errores y alertas que saltan y
// se resuelven solas.
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace zr::app {

class TrafficGenerator {
 public:
  explicit TrafficGenerator(uint64_t seed);

  // Añade n eventos NDJSON con marcas de tiempo en (now_ms - spread_ms, now_ms].
  void generate(size_t n, int64_t now_ms, int64_t spread_ms, std::string& out);

  // Servicio degradado en este instante (vacío si ninguno).
  static std::string incident_at(int64_t now_ms);

 private:
  struct Route {
    const char* service;
    const char* method;
    const char* path;
    double weight;
    double median_ms;
    double sigma;
    double error_rate;
  };

  const Route& pick_route();
  uint32_t pick_user();

  std::mt19937_64 rng_;
  std::vector<Route> routes_;
  std::vector<double> route_cdf_;
  std::vector<double> user_cdf_;
};

}  // namespace zr::app
