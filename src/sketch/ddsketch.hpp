// DDSketch (Masson, Rim y Lee, VLDB 2019): cuantiles con error RELATIVO
// garantizado. Con α = 1 %, el p99 estimado está a menos del 1 % del valor real
// sea cual sea la distribución, y dos sketches se fusionan sumando cubetas, que
// es justo lo que necesitan las ventanas por paneles y las particiones.
//
// Cada valor x > 0 cae en la cubeta k = ceil(ln(x) / ln(γ)), γ = (1+α)/(1-α).
// Las cubetas se guardan en un "store" híbrido: disperso (pares ordenados)
// mientras hay pocas, y denso (array contiguo con desplazamiento) después. El
// store denso tiene un tope de cubetas: si se supera, colapsa las más bajas
// (pierde precisión en los cuantiles más bajos, nunca en los altos).
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace zr::sketch {

class DDSketch {
 public:
  explicit DDSketch(double relative_accuracy = 0.01, uint32_t max_bins = 4096);

  void add(double v);
  // Fusiona otro sketch con la misma precisión relativa.
  void merge(const DDSketch& o);

  // Cuantil q en [0, 1]. NaN si el sketch está vacío.
  double quantile(double q) const;

  uint64_t count() const noexcept { return count_; }
  double min() const noexcept { return min_; }
  double max() const noexcept { return max_; }
  double sum() const noexcept { return sum_; }
  double relative_accuracy() const noexcept { return alpha_; }
  size_t memory_bytes() const noexcept;

 private:
  class Store {
   public:
    void add(int32_t key, uint64_t n, uint32_t max_bins);
    void merge(const Store& o, uint32_t max_bins);
    uint64_t total() const noexcept { return total_; }
    // Clave de la cubeta en la posición `rank` (0-based, claves ascendentes).
    int32_t key_at_rank(double rank) const noexcept;
    size_t memory_bytes() const noexcept;

   private:
    static constexpr size_t kSparseMax = 48;
    void to_dense(uint32_t max_bins);
    void dense_add(int32_t key, uint64_t n, uint32_t max_bins);

    bool sparse_ = true;
    std::vector<std::pair<int32_t, uint32_t>> sparse_bins_;  // ordenado por clave
    std::vector<uint32_t> bins_;                              // denso
    int32_t offset_ = 0;                                      // clave de bins_[0]
    uint64_t total_ = 0;
  };

  int32_t key(double v) const noexcept;
  double value(int32_t key) const noexcept;

  double alpha_;
  double gamma_;
  double ln_gamma_;
  double multiplier_;
  double min_indexable_;
  uint32_t max_bins_;
  Store pos_;
  Store neg_;
  uint64_t zero_count_ = 0;
  uint64_t count_ = 0;
  double sum_ = 0;
  double min_ = std::numeric_limits<double>::infinity();
  double max_ = -std::numeric_limits<double>::infinity();
};

}  // namespace zr::sketch
