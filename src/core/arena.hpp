// Asignador por bloques ("bump allocator").
//
// Reservar es mover un puntero; liberar es reset() del arena completo. Se usa
// para la memoria efímera de un lote de eventos (cadenas desescapadas, valores
// intermedios de la VM) y para las claves y estados de una ventana.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace zr {

class Arena {
 public:
  explicit Arena(size_t block_size = 64 * 1024) : block_size_(block_size) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  void* alloc(size_t n, size_t align = alignof(std::max_align_t)) {
    if (n == 0) n = 1;
    if (cur_ < blocks_.size()) {
      Block& b = blocks_[cur_];
      const size_t off = align_up(off_, align);
      if (off + n <= b.size) {
        off_ = off + n;
        used_ += n;
        return b.data.get() + off;
      }
      // Probar el siguiente bloque ya reservado (tras un reset()).
      while (++cur_ < blocks_.size()) {
        off_ = 0;
        if (n + align <= blocks_[cur_].size) return alloc(n, align);
      }
    }
    const size_t size = std::max(block_size_, n + align);
    blocks_.push_back(Block{std::make_unique<char[]>(size), size});
    cur_ = blocks_.size() - 1;
    off_ = 0;
    return alloc(n, align);
  }

  template <class T>
  T* alloc_array(size_t count) {
    return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
  }

  // Copia una cadena dentro del arena y devuelve una vista estable.
  std::string_view copy(std::string_view s) {
    if (s.empty()) return {};
    char* p = static_cast<char*>(alloc(s.size(), 1));
    std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
  }

  // Libera todo pero conserva los bloques para reutilizarlos.
  void reset() noexcept {
    cur_ = 0;
    off_ = 0;
    used_ = 0;
    // Evitar retener memoria desmesurada tras un pico: conservar solo 4 bloques.
    if (blocks_.size() > 4) blocks_.resize(4);
  }

  size_t bytes_used() const noexcept { return used_; }

  size_t bytes_reserved() const noexcept {
    size_t t = 0;
    for (const auto& b : blocks_) t += b.size;
    return t;
  }

 private:
  struct Block {
    std::unique_ptr<char[]> data;
    size_t size;
  };

  static size_t align_up(size_t v, size_t a) noexcept { return (v + a - 1) & ~(a - 1); }

  size_t block_size_;
  std::vector<Block> blocks_;
  size_t cur_ = 0;
  size_t off_ = 0;
  size_t used_ = 0;
};

}  // namespace zr
