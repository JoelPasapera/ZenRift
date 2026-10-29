// Primitivas léxicas de JSON compartidas por el parser DOM y por el escáner
// de eventos de la ruta caliente. Validan la gramática de RFC 8259 y el UTF-8
// de las cadenas.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zr::json::lex {

inline constexpr int kMaxDepth = 64;

struct ByteTable {
  uint8_t v[256];
};

// Tabla de bytes que interrumpen el recorrido rápido de una cadena:
// comilla, barra invertida, controles (< 0x20) y bytes no ASCII (>= 0x80).
inline constexpr ByteTable kStringSpecial = [] {
  ByteTable t{};
  for (int i = 0; i < 0x20; ++i) t.v[i] = 1;
  t.v[static_cast<uint8_t>('"')] = 1;
  t.v[static_cast<uint8_t>('\\')] = 1;
  for (int i = 0x80; i < 256; ++i) t.v[i] = 1;
  return t;
}();

inline const char* skip_ws(const char* p, const char* end) noexcept {
  while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  return p;
}

// `p` apunta a la comilla inicial. Si la cadena es válida deja `p` tras la
// comilla final, `raw` con los bytes entre comillas (aún escapados) y
// `has_escapes` indicando si hay secuencias '\'.
bool scan_string(const char*& p, const char* end, std::string_view& raw, bool& has_escapes) noexcept;

// Desescapa el contenido crudo de una cadena ya validada por scan_string.
// Los sustitutos UTF-16 sueltos se reemplazan por U+FFFD.
void unescape(std::string_view raw, std::string& out);
// Igual, escribiendo en un búfer ya reservado de al menos raw.size() bytes;
// devuelve los bytes escritos (el resultado nunca es más largo que la entrada).
size_t unescape_into(std::string_view raw, char* dst) noexcept;

// Número JSON. Deja `p` tras el número.
bool scan_number(const char*& p, const char* end, double& out) noexcept;

// Salta un valor completo de cualquier tipo.
bool skip_value(const char*& p, const char* end, int depth = 0) noexcept;

// Literales true/false/null.
bool match_literal(const char*& p, const char* end, std::string_view lit) noexcept;

}  // namespace zr::json::lex
