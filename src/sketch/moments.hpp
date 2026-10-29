// Momentos de primer y segundo orden (media y varianza) con el algoritmo de
// Welford, fusionables con la fórmula de Chan et al. Numéricamente estables a
// diferencia de acumular sum(x) y sum(x²).
#pragma once

#include <cmath>
#include <cstdint>

namespace zr::sketch {

struct Moments {
  uint64_t n = 0;
  double mean = 0;
  double m2 = 0;

  void add(double x) noexcept {
    ++n;
    const double d = x - mean;
    mean += d / static_cast<double>(n);
    m2 += d * (x - mean);
  }

  void merge(const Moments& o) noexcept {
    if (o.n == 0) return;
    if (n == 0) {
      *this = o;
      return;
    }
    const double na = static_cast<double>(n), nb = static_cast<double>(o.n);
    const double tot = na + nb;
    const double d = o.mean - mean;
    mean += d * nb / tot;
    m2 += o.m2 + d * d * na * nb / tot;
    n += o.n;
  }

  // Varianza muestral (n-1). Requiere n >= 2.
  double variance() const noexcept { return n > 1 ? m2 / static_cast<double>(n - 1) : 0.0; }
  double stddev() const noexcept { return std::sqrt(variance()); }
};

}  // namespace zr::sketch
