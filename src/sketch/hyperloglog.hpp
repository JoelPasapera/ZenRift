// HyperLogLog para contar valores distintos (usuarios únicos, IPs...).
//
// - Precisión p = 14: 16 384 registros, error típico ≈ 1,04/√m ≈ 0,8 %.
// - Estimador "mejorado" de Otmar Ertl (2017): sin tablas empíricas de sesgo y
//   preciso en todo el rango, desde cardinalidades minúsculas a miles de millones.
// - Modo disperso: con pocos registros tocados se guardan solo esos (mapa
//   índice -> valor); al pasar de un umbral se pasa al array denso de 16 KB.
//   Así un grupo con 3 usuarios no paga 16 KB.
// - Fusionable: máximo registro a registro.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/flat_map.hpp"

namespace zr::sketch {

class HyperLogLog {
 public:
  static constexpr int kP = 14;
  static constexpr uint32_t kM = 1u << kP;
  static constexpr int kQ = 64 - kP;  // bits restantes del hash
  static constexpr size_t kSparseMax = 768;

  HyperLogLog() = default;
  HyperLogLog(const HyperLogLog& o);
  HyperLogLog& operator=(const HyperLogLog& o);
  HyperLogLog(HyperLogLog&&) noexcept = default;
  HyperLogLog& operator=(HyperLogLog&&) noexcept = default;

  // Recibe un hash de 64 bits bien distribuido del valor.
  void add_hash(uint64_t h);
  void merge(const HyperLogLog& o);
  double estimate() const;
  bool is_sparse() const noexcept { return !dense_; }
  size_t memory_bytes() const noexcept;

 private:
  void set_register(uint32_t idx, uint8_t value);
  void to_dense();

  std::unique_ptr<uint8_t[]> dense_;
  FlatMap<uint32_t, uint8_t> sparse_;
};

}  // namespace zr::sketch
