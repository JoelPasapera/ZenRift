// Utilidades de texto sin dependencias: concatenación tipada, conversión de
// números con std::to_chars/from_chars (exactas y sin locale), tamaños ("64MB").
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace zr {

void append_int(std::string& out, int64_t v);
void append_uint(std::string& out, uint64_t v);
// Representación más corta que vuelve exactamente al mismo double. Enteros
// exactos se escriben sin decimales. NaN/Inf se escriben como "null" si
// json_safe es true.
void append_double(std::string& out, double v, bool json_safe = true);

namespace detail {

inline void append_one(std::string& out, std::string_view v) { out.append(v); }
inline void append_one(std::string& out, const char* v) { out.append(v); }
inline void append_one(std::string& out, const std::string& v) { out.append(v); }
inline void append_one(std::string& out, char v) { out.push_back(v); }
inline void append_one(std::string& out, bool v) { out.append(v ? "true" : "false"); }

template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
inline void append_one(std::string& out, T v) {
  if constexpr (std::is_signed_v<T>)
    append_int(out, static_cast<int64_t>(v));
  else
    append_uint(out, static_cast<uint64_t>(v));
}

template <class T>
  requires std::is_floating_point_v<T>
inline void append_one(std::string& out, T v) {
  append_double(out, static_cast<double>(v), false);
}

}  // namespace detail

template <class... A>
void append_all(std::string& out, const A&... a) {
  (detail::append_one(out, a), ...);
}

template <class... A>
std::string cat(const A&... a) {
  std::string s;
  append_all(s, a...);
  return s;
}

std::string_view trim(std::string_view s) noexcept;
bool iequals(std::string_view a, std::string_view b) noexcept;
std::string to_lower_ascii(std::string_view s);
std::vector<std::string_view> split(std::string_view s, char sep);

bool parse_int64(std::string_view s, int64_t& out) noexcept;
bool parse_double(std::string_view s, double& out) noexcept;
// "1048576", "512KB", "64MB", "1.5GB" (múltiplos de 1024).
bool parse_size(std::string_view s, uint64_t& out) noexcept;
std::string format_bytes(uint64_t n);

// Escapado JSON de una cadena (sin comillas exteriores).
void append_json_escaped(std::string& out, std::string_view s);

// Decodificación de componentes de URL (%XX y '+' como espacio).
std::string url_decode(std::string_view s);

}  // namespace zr
