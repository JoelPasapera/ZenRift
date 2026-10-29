// Estado de las agregaciones de un grupo.
//
// Todas las agregaciones de una consulta viven en un único bloque contiguo por
// grupo (count, sumas y momentos en línea; los sketches como punteros). Cada
// tipo de agregación es una tabla de funciones (sin herencia ni virtuales por
// evento): init, update, merge, copy, result y destroy. Que todas sean
// fusionables es lo que permite repartir el trabajo entre particiones y
// componer ventanas deslizantes a partir de paneles.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/arena.hpp"
#include "zql/plan.hpp"
#include "zql/value.hpp"

namespace zr::engine {

struct ResultContext {
  int64_t window_ms = 0;
  Arena* scratch = nullptr;
};

struct AggOps {
  uint32_t size;
  uint32_t align;
  void (*init)(void* st);
  void (*destroy)(void* st);
  void (*update)(void* st, const zql::Value& v, int64_t ts, const zql::AggSpec& spec);
  void (*merge)(void* dst, const void* src, const zql::AggSpec& spec);
  void (*copy)(void* dst, const void* src);  // dst sin inicializar
  zql::Value (*result)(const void* st, const zql::AggSpec& spec, const ResultContext& ctx);
};

const AggOps& agg_ops(zql::AggKind kind) noexcept;

// Capacidad de Space-Saving para un top-k: más contadores = más precisión.
uint32_t topk_capacity(uint32_t k) noexcept;

// Distribución del bloque de estados de una consulta.
class AggLayout {
 public:
  struct Entry {
    const AggOps* ops;
    uint32_t offset;
    const zql::AggSpec* spec;
  };

  explicit AggLayout(std::shared_ptr<const zql::QueryPlan> plan);

  uint32_t block_size() const noexcept { return block_size_; }
  uint32_t block_align() const noexcept { return block_align_; }
  const std::vector<Entry>& entries() const noexcept { return entries_; }
  const zql::QueryPlan& plan() const noexcept { return *plan_; }

  void init_block(char* block) const;
  void destroy_block(char* block) const;
  void copy_block(char* dst, const char* src) const;
  void merge_block(char* dst, const char* src) const;

 private:
  std::shared_ptr<const zql::QueryPlan> plan_;
  std::vector<Entry> entries_;
  uint32_t block_size_ = 0;
  uint32_t block_align_ = 8;
};

}  // namespace zr::engine
