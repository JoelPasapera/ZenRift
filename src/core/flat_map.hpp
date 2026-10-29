// Tabla hash de direccionamiento abierto con Robin Hood hashing.
//
// - Capacidad potencia de dos, factor de carga máximo 7/8.
// - Cada ranura guarda su distancia de sondeo (+1) en un byte aparte: 0 = vacía.
//   Robin Hood mantiene las distancias acotadas y la búsqueda puede cortarse en
//   cuanto la distancia de la ranura es menor que la del elemento buscado.
// - Borrado con desplazamiento hacia atrás (sin lápidas).
//
// Se usa en las tablas de grupos de las ventanas, en Space-Saving, en el modo
// disperso de HyperLogLog y en el disposición de campos de los eventos.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

#include "core/hash.hpp"

namespace zr {

template <class K, class V, class H = Hasher<K>, class E = std::equal_to<K>>
class FlatMap {
 public:
  struct Entry {
    K key;
    V value;
  };

  FlatMap() = default;
  explicit FlatMap(size_t expected) { reserve(expected); }

  ~FlatMap() { destroy_all(); }

  FlatMap(const FlatMap& other) {
    reserve(other.size_);
    other.for_each([this](const K& k, const V& v) { try_emplace(k, v); });
  }

  FlatMap& operator=(const FlatMap& other) {
    if (this != &other) {
      FlatMap tmp(other);
      swap(tmp);
    }
    return *this;
  }

  FlatMap(FlatMap&& other) noexcept { swap(other); }

  FlatMap& operator=(FlatMap&& other) noexcept {
    if (this != &other) {
      destroy_all();
      swap(other);
    }
    return *this;
  }

  void swap(FlatMap& o) noexcept {
    std::swap(dist_, o.dist_);
    std::swap(slots_, o.slots_);
    std::swap(cap_, o.cap_);
    std::swap(size_, o.size_);
  }

  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_t capacity() const noexcept { return cap_; }

  V* find(const K& key) noexcept {
    const size_t i = locate(key);
    return i == kNpos ? nullptr : &slots_[i].value;
  }

  const V* find(const K& key) const noexcept {
    const size_t i = locate(key);
    return i == kNpos ? nullptr : &slots_[i].value;
  }

  bool contains(const K& key) const noexcept { return locate(key) != kNpos; }

  // Inserta (key, V(args...)) si no existe. Devuelve el valor y si se insertó.
  template <class... Args>
  std::pair<V*, bool> try_emplace(const K& key, Args&&... args) {
    if (cap_ != 0) {
      const size_t found = locate(key);
      if (found != kNpos) return {&slots_[found].value, false};
    }
    if ((size_ + 1) * 8 > cap_ * 7) grow();
    Entry carry{key, V(std::forward<Args>(args)...)};
    const bool rehashed = insert_new(std::move(carry));
    if (!rehashed) return {&slots_[last_insert_].value, true};
    return {find(key), true};
  }

  V& operator[](const K& key) { return *try_emplace(key).first; }

  bool erase(const K& key) {
    size_t i = locate(key);
    if (i == kNpos) return false;
    erase_at(i);
    return true;
  }

  void clear() noexcept {
    if (!slots_) return;
    for (size_t i = 0; i < cap_; ++i) {
      if (dist_[i] != 0) {
        slots_[i].~Entry();
        dist_[i] = 0;
      }
    }
    size_ = 0;
  }

  void reserve(size_t n) {
    size_t need = 8;
    while (need * 7 < n * 8) need <<= 1;
    if (need > cap_) rehash(need);
  }

  template <class F>
  void for_each(F&& f) {
    for (size_t i = 0; i < cap_; ++i)
      if (dist_[i] != 0) f(slots_[i].key, slots_[i].value);
  }

  template <class F>
  void for_each(F&& f) const {
    for (size_t i = 0; i < cap_; ++i)
      if (dist_[i] != 0) f(static_cast<const K&>(slots_[i].key), static_cast<const V&>(slots_[i].value));
  }

 private:
  static constexpr size_t kNpos = static_cast<size_t>(-1);
  static constexpr uint8_t kMaxDist = 250;

  size_t locate(const K& key) const noexcept {
    if (cap_ == 0) return kNpos;
    const size_t mask = cap_ - 1;
    size_t i = static_cast<size_t>(H{}(key)) & mask;
    uint8_t d = 1;
    while (true) {
      const uint8_t sd = dist_[i];
      if (sd < d) return kNpos;  // vacía (0) o elemento más "rico": no está
      if (sd == d && E{}(slots_[i].key, key)) return i;
      i = (i + 1) & mask;
      ++d;
    }
  }

  // Inserta un elemento que se sabe ausente. Devuelve true si hubo rehash
  // (en ese caso last_insert_ no es válido y hay que volver a buscar).
  bool insert_new(Entry&& entry) {
    bool rehashed = false;
    bool placed_original = false;
    Entry carry(std::move(entry));
    while (true) {
      const size_t mask = cap_ - 1;
      size_t i = static_cast<size_t>(H{}(carry.key)) & mask;
      uint8_t d = 1;
      bool restart = false;
      while (true) {
        if (dist_[i] == 0) {
          ::new (static_cast<void*>(&slots_[i])) Entry(std::move(carry));
          dist_[i] = d;
          ++size_;
          if (!placed_original) last_insert_ = i;
          return rehashed;
        }
        if (dist_[i] < d) {
          std::swap(carry, slots_[i]);
          std::swap(d, dist_[i]);
          if (!placed_original) {
            last_insert_ = i;
            placed_original = true;
          }
        }
        i = (i + 1) & mask;
        if (++d >= kMaxDist) {
          // Distancia excesiva: crecer y reinsertar el elemento que llevamos.
          rehash(cap_ * 2);
          rehashed = true;
          restart = true;
          break;
        }
      }
      if (!restart) break;
    }
    return rehashed;
  }

  void erase_at(size_t i) noexcept {
    const size_t mask = cap_ - 1;
    slots_[i].~Entry();
    size_t next = (i + 1) & mask;
    while (dist_[next] > 1) {
      ::new (static_cast<void*>(&slots_[i])) Entry(std::move(slots_[next]));
      slots_[next].~Entry();
      dist_[i] = static_cast<uint8_t>(dist_[next] - 1);
      i = next;
      next = (next + 1) & mask;
    }
    dist_[i] = 0;
    --size_;
  }

  void grow() { rehash(cap_ == 0 ? 8 : cap_ * 2); }

  void rehash(size_t new_cap) {
    uint8_t* old_dist = dist_;
    Entry* old_slots = slots_;
    const size_t old_cap = cap_;

    dist_ = new uint8_t[new_cap];
    std::memset(dist_, 0, new_cap);
    slots_ = static_cast<Entry*>(::operator new(sizeof(Entry) * new_cap, std::align_val_t(alignof(Entry))));
    cap_ = new_cap;
    size_ = 0;

    for (size_t i = 0; i < old_cap; ++i) {
      if (old_dist[i] != 0) {
        insert_new(std::move(old_slots[i]));
        old_slots[i].~Entry();
      }
    }
    delete[] old_dist;
    if (old_slots) ::operator delete(old_slots, std::align_val_t(alignof(Entry)));
  }

  void destroy_all() noexcept {
    clear();
    delete[] dist_;
    if (slots_) ::operator delete(slots_, std::align_val_t(alignof(Entry)));
    dist_ = nullptr;
    slots_ = nullptr;
    cap_ = 0;
    size_ = 0;
  }

  uint8_t* dist_ = nullptr;
  Entry* slots_ = nullptr;
  size_t cap_ = 0;
  size_t size_ = 0;
  size_t last_insert_ = 0;
};

}  // namespace zr
