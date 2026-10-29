#include "core/strings.hpp"

#include <cmath>

namespace zr {

void append_int(std::string& out, int64_t v) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

void append_uint(std::string& out, uint64_t v) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

void append_double(std::string& out, double v, bool json_safe) {
  if (!std::isfinite(v)) {
    if (json_safe) {
      out.append("null");
    } else {
      out.append(std::isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf"));
    }
    return;
  }
  // Enteros exactos (hasta 2^53) sin parte decimal: más legibles y compactos.
  if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
    if (v == 0) {
      out.push_back('0');  // normaliza -0
      return;
    }
    append_int(out, static_cast<int64_t>(v));
    return;
  }
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

std::string_view trim(std::string_view s) noexcept {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
  }
  return true;
}

std::string to_lower_ascii(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return r;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (true) {
    const size_t p = s.find(sep, start);
    if (p == std::string_view::npos) {
      parts.push_back(s.substr(start));
      return parts;
    }
    parts.push_back(s.substr(start, p - start));
    start = p + 1;
  }
}

bool parse_int64(std::string_view s, int64_t& out) noexcept {
  if (s.empty()) return false;
  const char* b = s.data();
  if (*b == '+') ++b;
  auto r = std::from_chars(b, s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parse_double(std::string_view s, double& out) noexcept {
  if (s.empty()) return false;
  const char* b = s.data();
  if (*b == '+') ++b;
  auto r = std::from_chars(b, s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parse_size(std::string_view s, uint64_t& out) noexcept {
  s = trim(s);
  size_t i = 0;
  while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
  double v;
  if (!parse_double(s.substr(0, i), v) || v < 0) return false;
  std::string_view unit = trim(s.substr(i));
  double mult = 1;
  if (unit.empty() || iequals(unit, "b")) mult = 1;
  else if (iequals(unit, "k") || iequals(unit, "kb") || iequals(unit, "kib")) mult = 1024.0;
  else if (iequals(unit, "m") || iequals(unit, "mb") || iequals(unit, "mib")) mult = 1024.0 * 1024;
  else if (iequals(unit, "g") || iequals(unit, "gb") || iequals(unit, "gib")) mult = 1024.0 * 1024 * 1024;
  else if (iequals(unit, "t") || iequals(unit, "tb") || iequals(unit, "tib")) mult = 1024.0 * 1024 * 1024 * 1024;
  else return false;
  const double total = v * mult;
  if (total > 1.8e19) return false;
  out = static_cast<uint64_t>(total);
  return true;
}

std::string format_bytes(uint64_t n) {
  static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int u = 0;
  while (v >= 1024 && u < 4) {
    v /= 1024;
    ++u;
  }
  std::string s;
  if (u == 0) {
    append_uint(s, n);
  } else {
    append_double(s, std::round(v * 10) / 10, false);
  }
  s.push_back(' ');
  s.append(units[u]);
  return s;
}

void append_json_escaped(std::string& out, std::string_view s) {
  static constexpr char hex[] = "0123456789abcdef";
  size_t run = 0;  // inicio del tramo que no necesita escape
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    out.append(s.data() + run, i - run);
    run = i + 1;
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      default:
        out.append("\\u00");
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
  }
  out.append(s.data() + run, s.size() - run);
}

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < s.size()) {
      const int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace zr
