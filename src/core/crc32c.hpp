// CRC32C (Castagnoli). Protege cada registro del log en disco.
//
// Implementación por software "slicing-by-8" con tablas generadas en tiempo de
// compilación, y ruta por hardware (instrucción crc32 de SSE4.2) elegida en
// tiempo de ejecución cuando la CPU la tiene.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zr {

// Encadenable: crc32c(b, nb, crc32c(a, na)) == crc32c(a+b).
uint32_t crc32c(const void* data, size_t n, uint32_t crc = 0) noexcept;

// Solo la ruta por software (para pruebas y comparaciones).
uint32_t crc32c_sw(const void* data, size_t n, uint32_t crc = 0) noexcept;

// true si se está usando la instrucción por hardware.
bool crc32c_hw_available() noexcept;

}  // namespace zr
