#include "json/lex.hpp"

#include <charconv>

namespace zr::json::lex {

namespace {

inline bool is_cont(uint8_t c) noexcept { return (c & 0xC0) == 0x80; }

// Valida una secuencia UTF-8 que empieza en p (byte >= 0x80). Devuelve su
// longitud o 0 si es inválida.
size_t utf8_len(const uint8_t* p, const uint8_t* end) noexcept {
  const uint8_t c = *p;
  if (c < 0xC2) return 0;
  if (c < 0xE0) return (end - p >= 2 && is_cont(p[1])) ? 2 : 0;
  if (c < 0xF0) {
    if (end - p < 3) return 0;
    const uint8_t c1 = p[1];
    if (c == 0xE0 && (c1 < 0xA0 || c1 > 0xBF)) return 0;
    if (c == 0xED && (c1 < 0x80 || c1 > 0x9F)) return 0;  // sustitutos UTF-16
    if (!is_cont(c1) || !is_cont(p[2])) return 0;
    return 3;
  }
  if (c < 0xF5) {
    if (end - p < 4) return 0;
    const uint8_t c1 = p[1];
    if (c == 0xF0 && (c1 < 0x90 || c1 > 0xBF)) return 0;
    if (c == 0xF4 && (c1 < 0x80 || c1 > 0x8F)) return 0;
    if (!is_cont(c1) || !is_cont(p[2]) || !is_cont(p[3])) return 0;
    return 4;
  }
  return 0;
}

inline int hexval(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline uint32_t read_hex4(const char* p) noexcept {
  return static_cast<uint32_t>((hexval(p[0]) << 12) | (hexval(p[1]) << 8) | (hexval(p[2]) << 4) | hexval(p[3]));
}

inline size_t put_utf8(char* dst, uint32_t cp) noexcept {
  if (cp < 0x80) {
    dst[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    dst[0] = static_cast<char>(0xC0 | (cp >> 6));
    dst[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = static_cast<char>(0xE0 | (cp >> 12));
    dst[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = static_cast<char>(0xF0 | (cp >> 18));
  dst[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

}  // namespace

bool scan_string(const char*& p, const char* end, std::string_view& raw, bool& has_escapes) noexcept {
  // *p == '"'
  const char* s = ++p;
  has_escapes = false;
  while (true) {
    while (p < end && !kStringSpecial.v[static_cast<uint8_t>(*p)]) ++p;
    if (p >= end) return false;
    const uint8_t c = static_cast<uint8_t>(*p);
    if (c == '"') {
      raw = std::string_view(s, static_cast<size_t>(p - s));
      ++p;
      return true;
    }
    if (c == '\\') {
      has_escapes = true;
      if (p + 1 >= end) return false;
      const char e = p[1];
      if (e == 'u') {
        if (end - p < 6) return false;
        for (int i = 2; i < 6; ++i)
          if (hexval(p[i]) < 0) return false;
        p += 6;
      } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
        p += 2;
      } else {
        return false;
      }
      continue;
    }
    if (c < 0x20) return false;
    const size_t n = utf8_len(reinterpret_cast<const uint8_t*>(p), reinterpret_cast<const uint8_t*>(end));
    if (n == 0) return false;
    p += n;
  }
}

size_t unescape_into(std::string_view raw, char* dst) noexcept {
  const char* p = raw.data();
  const char* end = p + raw.size();
  char* o = dst;
  while (p < end) {
    const char c = *p;
    if (c != '\\') {
      *o++ = c;
      ++p;
      continue;
    }
    const char e = p[1];
    p += 2;
    switch (e) {
      case '"': *o++ = '"'; break;
      case '\\': *o++ = '\\'; break;
      case '/': *o++ = '/'; break;
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case 'n': *o++ = '\n'; break;
      case 'r': *o++ = '\r'; break;
      case 't': *o++ = '\t'; break;
      case 'u': {
        uint32_t cp = read_hex4(p);
        p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // Sustituto alto: debe seguir uno bajo.
          if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            const uint32_t lo = read_hex4(p + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            } else {
              cp = 0xFFFD;
            }
          } else {
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        o += put_utf8(o, cp);
        break;
      }
      default: break;  // inalcanzable: scan_string ya validó
    }
  }
  return static_cast<size_t>(o - dst);
}

void unescape(std::string_view raw, std::string& out) {
  const size_t base = out.size();
  out.resize(base + raw.size());
  const size_t n = unescape_into(raw, out.data() + base);
  out.resize(base + n);
}

bool scan_number(const char*& p, const char* end, double& out) noexcept {
  const char* s = p;
  const char* q = p;
  if (q < end && *q == '-') ++q;
  if (q >= end) return false;
  if (*q == '0') {
    ++q;
  } else if (*q >= '1' && *q <= '9') {
    while (q < end && *q >= '0' && *q <= '9') ++q;
  } else {
    return false;
  }
  if (q < end && *q == '.') {
    ++q;
    const char* d = q;
    while (q < end && *q >= '0' && *q <= '9') ++q;
    if (q == d) return false;
  }
  if (q < end && (*q == 'e' || *q == 'E')) {
    ++q;
    if (q < end && (*q == '+' || *q == '-')) ++q;
    const char* d = q;
    while (q < end && *q >= '0' && *q <= '9') ++q;
    if (q == d) return false;
  }
  auto r = std::from_chars(s, q, out);
  if (r.ec != std::errc() || r.ptr != q) return false;
  p = q;
  return true;
}

bool match_literal(const char*& p, const char* end, std::string_view lit) noexcept {
  if (static_cast<size_t>(end - p) < lit.size()) return false;
  for (size_t i = 0; i < lit.size(); ++i)
    if (p[i] != lit[i]) return false;
  p += lit.size();
  return true;
}

bool skip_value(const char*& p, const char* end, int depth) noexcept {
  if (depth > kMaxDepth) return false;
  p = skip_ws(p, end);
  if (p >= end) return false;
  switch (*p) {
    case '"': {
      std::string_view raw;
      bool esc;
      return scan_string(p, end, raw, esc);
    }
    case '{': {
      ++p;
      p = skip_ws(p, end);
      if (p < end && *p == '}') {
        ++p;
        return true;
      }
      while (true) {
        p = skip_ws(p, end);
        if (p >= end || *p != '"') return false;
        std::string_view raw;
        bool esc;
        if (!scan_string(p, end, raw, esc)) return false;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return false;
        ++p;
        if (!skip_value(p, end, depth + 1)) return false;
        p = skip_ws(p, end);
        if (p >= end) return false;
        if (*p == ',') {
          ++p;
          continue;
        }
        if (*p == '}') {
          ++p;
          return true;
        }
        return false;
      }
    }
    case '[': {
      ++p;
      p = skip_ws(p, end);
      if (p < end && *p == ']') {
        ++p;
        return true;
      }
      while (true) {
        if (!skip_value(p, end, depth + 1)) return false;
        p = skip_ws(p, end);
        if (p >= end) return false;
        if (*p == ',') {
          ++p;
          continue;
        }
        if (*p == ']') {
          ++p;
          return true;
        }
        return false;
      }
    }
    case 't': return match_literal(p, end, "true");
    case 'f': return match_literal(p, end, "false");
    case 'n': return match_literal(p, end, "null");
    default: {
      double d;
      return scan_number(p, end, d);
    }
  }
}

}  // namespace zr::json::lex
