// Posiciones en el texto fuente y errores con línea y columna, para que el
// editor del panel pueda señalar exactamente dónde está el problema.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zr::zql {

struct SourcePos {
  size_t offset = 0;
  int line = 1;
  int col = 1;
};

struct Diagnostic {
  SourcePos pos;
  std::string message;

  // "línea 2, columna 14: se esperaba FROM"
  std::string to_string() const;
};

// Calcula línea y columna de un desplazamiento (columnas en caracteres UTF-8).
SourcePos position_of(std::string_view src, size_t offset) noexcept;

}  // namespace zr::zql
