// Space-Saving (Metwally, Agrawal y El Abbadi, 2005) para los "top-k":
// rutas más visitadas, clientes más activos...
//
// Mantiene m contadores. Un elemento nuevo con la tabla llena reemplaza al de
// menor cuenta heredando esa cuenta como error. Garantías: cualquier elemento
// con frecuencia > N/m está monitorizado, y para cada contador
// cuenta - error <= frecuencia real <= cuenta.
//
// Estructuras propias: los contadores viven en un array que nunca se realoja
// (las vistas a sus claves son estables), un montículo binario de índices
// ordena por cuenta, y un FlatMap clave -> contador da la búsqueda en O(1).
// La fusión sigue a Cafaro et al.: suma de estimaciones, rellenando con el
// mínimo del otro resumen cuando el elemento no está, y se quedan los m mayores.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/flat_map.hpp"

namespace zr::sketch {

class SpaceSaving {
 public:
  struct Entry {
    std::string item;
    uint64_t count;
    uint64_t error;
  };

  explicit SpaceSaving(uint32_t capacity = 64);
  SpaceSaving(const SpaceSaving& o);
  SpaceSaving& operator=(const SpaceSaving& o);
  SpaceSaving(SpaceSaving&&) noexcept = default;
  SpaceSaving& operator=(SpaceSaving&&) noexcept = default;

  // `item` son bytes opacos (el llamador codifica el valor).
  void add(std::string_view item, uint64_t count = 1);
  void merge(const SpaceSaving& o);

  // Los k elementos de mayor cuenta, en orden descendente.
  std::vector<Entry> top(size_t k) const;

  uint64_t total() const noexcept { return total_; }
  uint32_t capacity() const noexcept { return capacity_; }
  size_t size() const noexcept { return heap_.size(); }
  size_t memory_bytes() const noexcept;

 private:
  struct Counter {
    std::string item;
    uint64_t count = 0;
    uint64_t error = 0;
  };

  uint64_t min_count() const noexcept { return heap_.empty() ? 0 : counters_[heap_[0]].count; }
  bool full() const noexcept { return heap_.size() >= capacity_; }
  void sift_down(size_t pos);
  void sift_up(size_t pos);
  void swap_heap(size_t a, size_t b);
  void rebuild(std::vector<Entry>&& entries);

  uint32_t capacity_;
  uint64_t total_ = 0;
  std::vector<Counter> counters_;          // tamaño fijo = capacity_, no se realoja
  std::vector<uint32_t> heap_;             // índices de counters_, montículo por cuenta
  std::vector<uint32_t> heap_pos_;         // contador -> posición en heap_
  FlatMap<std::string_view, uint32_t> index_;  // clave -> contador
};

}  // namespace zr::sketch
