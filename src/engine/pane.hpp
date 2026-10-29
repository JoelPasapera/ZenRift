// Panel: el estado agregado de un intervalo de tiempo [start, end), por grupo.
//
// Una ventana tumbling es un panel; una hopping de 1m cada 10s es la fusión de
// 6 paneles de 10s. Cada partición llena sus propios paneles y el coordinador
// los fusiona: por eso un panel sabe fusionarse con otro y clonarse.
//
// La tabla de grupos es un FlatMap (Robin Hood) de clave codificada -> índice;
// claves y bloques de estado viven en un arena propio.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/flat_map.hpp"
#include "engine/aggregate.hpp"

namespace zr::engine {

class Pane {
 public:
  Pane(std::shared_ptr<const AggLayout> layout, int64_t start, int64_t end);
  ~Pane();
  Pane(const Pane&) = delete;
  Pane& operator=(const Pane&) = delete;

  // Bloque de estado del grupo `key` (lo crea si no existe). nullptr si se
  // alcanzó el máximo de grupos.
  char* group(std::string_view key, uint64_t hash, size_t max_groups);

  void merge_from(const Pane& other, size_t max_groups);
  std::unique_ptr<Pane> clone() const;

  int64_t start() const noexcept { return start_; }
  int64_t end() const noexcept { return end_; }
  size_t groups() const noexcept { return groups_.size(); }
  const AggLayout& layout() const noexcept { return *layout_; }
  const std::shared_ptr<const AggLayout>& layout_ptr() const noexcept { return layout_; }

  template <class F>
  void for_each(F&& f) const {
    for (const auto& g : groups_) f(g.key, static_cast<const char*>(g.block));
  }

  uint64_t events = 0;          // eventos agregados
  uint64_t dropped_groups = 0;  // eventos descartados por exceder el máximo de grupos

 private:
  struct KeyRef {
    std::string_view bytes;
    uint64_t hash;
    bool operator==(const KeyRef& o) const noexcept { return hash == o.hash && bytes == o.bytes; }
  };
  struct KeyHash {
    uint64_t operator()(const KeyRef& k) const noexcept { return k.hash; }
  };
  struct Group {
    std::string_view key;
    uint64_t hash;
    char* block;
  };

  std::shared_ptr<const AggLayout> layout_;
  int64_t start_;
  int64_t end_;
  Arena arena_;
  FlatMap<KeyRef, uint32_t, KeyHash> index_;
  std::vector<Group> groups_;
};

}  // namespace zr::engine
