// Arnés mínimo de microbenchmarks: mide una función repitiéndola hasta
// acumular un tiempo mínimo, se queda con la mejor de tres tandas (la menos
// perturbada por el resto del sistema) e imprime coste por operación y
// rendimiento.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace zrb {

// Impide que el optimizador elimine un cálculo cuyo resultado no se usa.
template <class T>
inline void keep(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(&value) : "memory");
#else
  static const void* volatile sink;
  sink = &value;
#endif
}

struct Result {
  std::string name;
  double seconds_per_call = 0;  // mejor tanda
  uint64_t items_per_call = 1;
  uint64_t bytes_per_call = 0;
  std::string note;
};

inline double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// `fn` procesa `items` elementos (y `bytes` bytes, si aplica) en cada llamada.
template <class F>
Result measure(std::string name, uint64_t items, uint64_t bytes, F&& fn, double min_seconds = 0.3) {
  fn();  // calentamiento: cachés, predictores y asignaciones iniciales
  double best = 1e300;
  for (int round = 0; round < 3; ++round) {
    uint64_t calls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    double elapsed = 0;
    do {
      fn();
      ++calls;
      elapsed = seconds_since(t0);
    } while (elapsed < min_seconds);
    best = std::min(best, elapsed / static_cast<double>(calls));
  }
  return Result{std::move(name), best, items, bytes, {}};
}

inline void print_header() {
  std::printf("%-26s %12s %14s %12s  %s\n", "benchmark", "ns/op", "op/s", "MB/s", "notas");
  std::printf("%-26s %12s %14s %12s  %s\n", "--------------------------", "------------", "--------------",
              "------------", "-----");
}

inline std::string human_rate(double per_second) {
  char buf[32];
  if (per_second >= 1e9)
    std::snprintf(buf, sizeof buf, "%.2f G", per_second / 1e9);
  else if (per_second >= 1e6)
    std::snprintf(buf, sizeof buf, "%.2f M", per_second / 1e6);
  else if (per_second >= 1e3)
    std::snprintf(buf, sizeof buf, "%.1f k", per_second / 1e3);
  else
    std::snprintf(buf, sizeof buf, "%.1f", per_second);
  return buf;
}

inline void print(const Result& r) {
  const double per_item = r.seconds_per_call / static_cast<double>(r.items_per_call);
  char mbs[32] = "";
  if (r.bytes_per_call > 0)
    std::snprintf(mbs, sizeof mbs, "%.1f", static_cast<double>(r.bytes_per_call) / r.seconds_per_call / 1048576.0);
  std::printf("%-26s %12.1f %14s %12s  %s\n", r.name.c_str(), per_item * 1e9, human_rate(1.0 / per_item).c_str(), mbs,
              r.note.c_str());
  std::fflush(stdout);
}

}  // namespace zrb
