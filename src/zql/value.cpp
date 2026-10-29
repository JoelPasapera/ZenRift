#include "zql/value.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/strings.hpp"

namespace zr::zql {

const char* type_name(VType t) noexcept {
  switch (t) {
    case VType::Null: return "null";
    case VType::Bool: return "booleano";
    case VType::Num: return "número";
    case VType::Str: return "cadena";
    case VType::Json: return "json";
  }
  return "?";
}

namespace {

void put_varint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

bool get_varint(std::string_view b, size_t& pos, uint64_t& v) noexcept {
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos >= b.size()) return false;
    const uint8_t c = static_cast<uint8_t>(b[pos++]);
    v |= static_cast<uint64_t>(c & 0x7F) << shift;
    if (!(c & 0x80)) return true;
  }
  return false;
}

}  // namespace

void encode_value(const Value& v, std::string& out) {
  switch (v.type) {
    case VType::Null: out.push_back('n'); break;
    case VType::Bool:
      out.push_back('b');
      out.push_back(v.num != 0 ? '1' : '0');
      break;
    case VType::Num: {
      out.push_back('d');
      double d = v.num == 0 ? 0.0 : v.num;  // -0 -> 0
      if (d != d) d = std::numeric_limits<double>::quiet_NaN();
      const uint64_t bits = std::bit_cast<uint64_t>(d);
      char buf[8];
      std::memcpy(buf, &bits, 8);
      out.append(buf, 8);
      break;
    }
    case VType::Str:
    case VType::Json:
      out.push_back(v.type == VType::Str ? 's' : 'j');
      put_varint(out, v.len);
      out.append(v.s, v.len);
      break;
  }
}

bool decode_value(std::string_view b, size_t& pos, Value& out) noexcept {
  if (pos >= b.size()) return false;
  const char tag = b[pos++];
  switch (tag) {
    case 'n': out = Value::null(); return true;
    case 'b':
      if (pos >= b.size()) return false;
      out = Value::boolean(b[pos++] == '1');
      return true;
    case 'd': {
      if (pos + 8 > b.size()) return false;
      uint64_t bits;
      std::memcpy(&bits, b.data() + pos, 8);
      pos += 8;
      out = Value::number(std::bit_cast<double>(bits));
      return true;
    }
    case 's':
    case 'j': {
      uint64_t len;
      if (!get_varint(b, pos, len) || pos + len > b.size()) return false;
      const std::string_view sv = b.substr(pos, static_cast<size_t>(len));
      pos += static_cast<size_t>(len);
      out = tag == 's' ? Value::string(sv) : Value::json(sv);
      return true;
    }
    default: return false;
  }
}

void write_json(std::string& out, const Value& v) {
  switch (v.type) {
    case VType::Null: out += "null"; break;
    case VType::Bool: out += v.num != 0 ? "true" : "false"; break;
    case VType::Num: append_double(out, v.num); break;
    case VType::Str:
      out.push_back('"');
      append_json_escaped(out, v.str());
      out.push_back('"');
      break;
    case VType::Json: out.append(v.s, v.len); break;
  }
}

void append_text(std::string& out, const Value& v) {
  switch (v.type) {
    case VType::Null: out += "null"; break;
    case VType::Bool: out += v.num != 0 ? "true" : "false"; break;
    case VType::Num: append_double(out, v.num, false); break;
    case VType::Str:
    case VType::Json: out.append(v.s, v.len); break;
  }
}

Tri equals(const Value& a, const Value& b) noexcept {
  if (a.type == VType::Null || b.type == VType::Null) return Tri::Unknown;
  const bool an = a.type == VType::Num || a.type == VType::Bool;
  const bool bn = b.type == VType::Num || b.type == VType::Bool;
  if (an && bn) {
    if (a.num != a.num || b.num != b.num) return Tri::Unknown;
    return a.num == b.num ? Tri::True : Tri::False;
  }
  if ((a.type == VType::Str || a.type == VType::Json) && (b.type == VType::Str || b.type == VType::Json))
    return a.str() == b.str() ? Tri::True : Tri::False;
  return Tri::False;  // tipos distintos: nunca iguales
}

int compare(const Value& a, const Value& b, bool& ok) noexcept {
  ok = false;
  if (a.type == VType::Null || b.type == VType::Null) return 0;
  const bool an = a.type == VType::Num || a.type == VType::Bool;
  const bool bn = b.type == VType::Num || b.type == VType::Bool;
  if (an && bn) {
    if (a.num != a.num || b.num != b.num) return 0;
    ok = true;
    return a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
  }
  if (a.type == VType::Str && b.type == VType::Str) {
    ok = true;
    const int c = a.str().compare(b.str());
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  return 0;
}

int total_order(const Value& a, const Value& b) noexcept {
  auto rank = [](VType t) {
    switch (t) {
      case VType::Null: return 0;
      case VType::Bool: return 1;
      case VType::Num: return 2;
      case VType::Str: return 3;
      case VType::Json: return 4;
    }
    return 5;
  };
  const int ra = rank(a.type), rb = rank(b.type);
  if (ra != rb) return ra < rb ? -1 : 1;
  switch (a.type) {
    case VType::Null: return 0;
    case VType::Bool:
    case VType::Num: {
      const bool na = a.num != a.num, nb = b.num != b.num;
      if (na || nb) return na == nb ? 0 : (na ? -1 : 1);
      return a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
    }
    default: {
      const int c = a.str().compare(b.str());
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
  }
}

}  // namespace zr::zql
