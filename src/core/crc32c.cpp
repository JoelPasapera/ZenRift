#include "core/crc32c.hpp"

#include <array>
#include <cstring>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define ZR_CRC_HW 1
#include <nmmintrin.h>
#endif

namespace zr {

namespace {

constexpr uint32_t kPoly = 0x82F63B78u;  // polinomio de Castagnoli, reflejado

using Tables = std::array<std::array<uint32_t, 256>, 8>;

constexpr Tables make_tables() {
  Tables t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ kPoly : c >> 1;
    t[0][i] = c;
  }
  for (size_t k = 1; k < 8; ++k)
    for (uint32_t i = 0; i < 256; ++i) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xff];
  return t;
}

constexpr Tables kTables = make_tables();

uint32_t sw_update(uint32_t c, const uint8_t* p, size_t n) noexcept {
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);  // asume little-endian (x86-64, ARM64)
    w ^= c;
    c = kTables[7][w & 0xff] ^ kTables[6][(w >> 8) & 0xff] ^ kTables[5][(w >> 16) & 0xff] ^
        kTables[4][(w >> 24) & 0xff] ^ kTables[3][(w >> 32) & 0xff] ^ kTables[2][(w >> 40) & 0xff] ^
        kTables[1][(w >> 48) & 0xff] ^ kTables[0][w >> 56];
    p += 8;
    n -= 8;
  }
  while (n--) c = (c >> 8) ^ kTables[0][(c ^ *p++) & 0xff];
  return c;
}

#ifdef ZR_CRC_HW
__attribute__((target("sse4.2"))) uint32_t hw_update(uint32_t c, const uint8_t* p, size_t n) noexcept {
  uint64_t c64 = c;
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    c64 = _mm_crc32_u64(c64, w);
    p += 8;
    n -= 8;
  }
  c = static_cast<uint32_t>(c64);
  while (n--) c = _mm_crc32_u8(c, *p++);
  return c;
}

bool detect_hw() noexcept {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2");
}

const bool g_hw = detect_hw();
#endif

}  // namespace

uint32_t crc32c_sw(const void* data, size_t n, uint32_t crc) noexcept {
  return ~sw_update(~crc, static_cast<const uint8_t*>(data), n);
}

uint32_t crc32c(const void* data, size_t n, uint32_t crc) noexcept {
#ifdef ZR_CRC_HW
  if (g_hw) return ~hw_update(~crc, static_cast<const uint8_t*>(data), n);
#endif
  return crc32c_sw(data, n, crc);
}

bool crc32c_hw_available() noexcept {
#ifdef ZR_CRC_HW
  return g_hw;
#else
  return false;
#endif
}

}  // namespace zr
