// Hash de 64 bits de propósito general (no criptográfico).
//
// Mezcla por multiplicación 64x64 -> 128 bits ("mum"), en la línea de wyhash
// (dominio público), escrita de cero. Se usa para las tablas hash del motor,
// las claves de grupo y los sketches (HyperLogLog necesita bits bien
// distribuidos, así que la calidad del hash importa).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace zr {

namespace detail {

inline constexpr uint64_t kHashSecret[4] = {
    0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull,
    0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull};

inline void mum(uint64_t& a, uint64_t& b) noexcept {
#if defined(__SIZEOF_INT128__)
  __uint128_t r = static_cast<__uint128_t>(a) * b;
  a = static_cast<uint64_t>(r);
  b = static_cast<uint64_t>(r >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
  a = _umul128(a, b, &b);
#else
  // Multiplicación 64x64 -> 128 portable a partir de mitades de 32 bits.
  const uint64_t ha = a >> 32, hb = b >> 32, la = a & 0xffffffffu, lb = b & 0xffffffffu;
  const uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
  const uint64_t t = rl + (rm0 << 32);
  uint64_t c = t < rl;
  const uint64_t lo = t + (rm1 << 32);
  c += lo < t;
  const uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
  a = lo;
  b = hi;
#endif
}

inline uint64_t mix(uint64_t a, uint64_t b) noexcept {
  mum(a, b);
  return a ^ b;
}

inline uint64_t read64(const uint8_t* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

inline uint64_t read32(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline uint64_t read_small(const uint8_t* p, size_t k) noexcept {
  return (static_cast<uint64_t>(p[0]) << 16) | (static_cast<uint64_t>(p[k >> 1]) << 8) | p[k - 1];
}

}  // namespace detail

// Hash de un bloque de bytes arbitrario.
inline uint64_t hash_bytes(const void* data, size_t len, uint64_t seed = 0) noexcept {
  using namespace detail;
  const auto* p = static_cast<const uint8_t*>(data);
  seed ^= mix(seed ^ kHashSecret[0], kHashSecret[1]);
  uint64_t a = 0, b = 0;
  if (len <= 16) {
    if (len >= 4) {
      a = (read32(p) << 32) | read32(p + ((len >> 3) << 2));
      b = (read32(p + len - 4) << 32) | read32(p + len - 4 - ((len >> 3) << 2));
    } else if (len > 0) {
      a = read_small(p, len);
    }
  } else {
    size_t i = len;
    if (i > 48) {
      uint64_t s1 = seed, s2 = seed;
      do {
        seed = mix(read64(p) ^ kHashSecret[1], read64(p + 8) ^ seed);
        s1 = mix(read64(p + 16) ^ kHashSecret[2], read64(p + 24) ^ s1);
        s2 = mix(read64(p + 32) ^ kHashSecret[3], read64(p + 40) ^ s2);
        p += 48;
        i -= 48;
      } while (i > 48);
      seed ^= s1 ^ s2;
    }
    while (i > 16) {
      seed = mix(read64(p) ^ kHashSecret[1], read64(p + 8) ^ seed);
      i -= 16;
      p += 16;
    }
    a = read64(p + i - 16);
    b = read64(p + i - 8);
  }
  a ^= kHashSecret[1];
  b ^= seed;
  mum(a, b);
  return mix(a ^ kHashSecret[0] ^ len, b ^ kHashSecret[1]);
}

inline uint64_t hash_str(std::string_view s, uint64_t seed = 0) noexcept {
  return hash_bytes(s.data(), s.size(), seed);
}

// Finalizador para enteros (splitmix64): barato y con buena avalancha.
inline uint64_t hash_u64(uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// Combina dos hashes (orden importa).
inline uint64_t hash_combine(uint64_t h, uint64_t v) noexcept {
  return detail::mix(h ^ detail::kHashSecret[2], v ^ detail::kHashSecret[3]);
}

// Functores para FlatMap.
template <class T>
struct Hasher;

template <>
struct Hasher<std::string_view> {
  uint64_t operator()(std::string_view s) const noexcept { return hash_str(s); }
};

template <>
struct Hasher<uint64_t> {
  uint64_t operator()(uint64_t v) const noexcept { return hash_u64(v); }
};

template <>
struct Hasher<uint32_t> {
  uint64_t operator()(uint32_t v) const noexcept { return hash_u64(v); }
};

template <>
struct Hasher<int64_t> {
  uint64_t operator()(int64_t v) const noexcept { return hash_u64(static_cast<uint64_t>(v)); }
};

}  // namespace zr
