#include "zql/program.hpp"

#include "core/strings.hpp"

namespace zr::zql {

const char* opcode_name(OpCode op) noexcept {
  switch (op) {
    case OpCode::Const: return "CONST";
    case OpCode::Field: return "FIELD";
    case OpCode::Key: return "KEY";
    case OpCode::Agg: return "AGG";
    case OpCode::Col: return "COL";
    case OpCode::WinStart: return "WIN_START";
    case OpCode::WinEnd: return "WIN_END";
    case OpCode::WinSecs: return "WIN_SECS";
    case OpCode::Neg: return "NEG";
    case OpCode::Not: return "NOT";
    case OpCode::Add: return "ADD";
    case OpCode::Sub: return "SUB";
    case OpCode::Mul: return "MUL";
    case OpCode::Div: return "DIV";
    case OpCode::Mod: return "MOD";
    case OpCode::Concat: return "CONCAT";
    case OpCode::Eq: return "EQ";
    case OpCode::Ne: return "NE";
    case OpCode::Lt: return "LT";
    case OpCode::Le: return "LE";
    case OpCode::Gt: return "GT";
    case OpCode::Ge: return "GE";
    case OpCode::And: return "AND";
    case OpCode::Or: return "OR";
    case OpCode::JumpIfFalse: return "JMP_IF_FALSE";
    case OpCode::JumpIfTrue: return "JMP_IF_TRUE";
    case OpCode::JumpIfNotTrue: return "JMP_IF_NOT_TRUE";
    case OpCode::Jump: return "JMP";
    case OpCode::Pop: return "POP";
    case OpCode::IsNull: return "IS_NULL";
    case OpCode::IsNotNull: return "IS_NOT_NULL";
    case OpCode::InSet: return "IN_SET";
    case OpCode::Like: return "LIKE";
    case OpCode::Call: return "CALL";
    case OpCode::Ret: return "RET";
  }
  return "?";
}

void ValueSet::add(const Value& v) {
  std::string k;
  encode_value(v, k);
  items_.push_back(std::move(k));
}

void ValueSet::seal() {
  index_.clear();
  index_.reserve(items_.size());
  for (const auto& s : items_) index_.try_emplace(std::string_view(s), uint8_t{1});
}

bool ValueSet::contains(const Value& v) const {
  // Los números se comparan por valor: true/false cuentan como 1/0.
  thread_local std::string key;
  key.clear();
  if (v.type == VType::Bool) {
    encode_value(Value::number(v.num), key);
    if (index_.contains(key)) return true;
    key.clear();
  }
  encode_value(v, key);
  return index_.contains(key);
}

namespace {

inline char fold(char c, bool ci) noexcept { return ci && c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// Longitud del carácter UTF-8 que empieza en c.
inline size_t char_len(unsigned char c) noexcept {
  if (c < 0x80) return 1;
  if (c < 0xE0) return 2;
  if (c < 0xF0) return 3;
  return 4;
}

}  // namespace

bool LikePattern::matches(std::string_view s) const noexcept {
  // Algoritmo iterativo de comodines con un único punto de retroceso: lineal
  // en la práctica y sin recursión.
  const std::string_view p = pattern;
  size_t si = 0, pi = 0;
  size_t star_p = std::string_view::npos, star_s = 0;
  while (si < s.size()) {
    if (pi < p.size() && p[pi] == '_') {
      si += char_len(static_cast<unsigned char>(s[si]));
      ++pi;
    } else if (pi < p.size() && p[pi] == '%') {
      star_p = pi++;
      star_s = si;
    } else if (pi < p.size() && fold(p[pi], case_insensitive) == fold(s[si], case_insensitive)) {
      ++si;
      ++pi;
    } else if (star_p != std::string_view::npos) {
      pi = star_p + 1;
      star_s += char_len(static_cast<unsigned char>(s[star_s]));
      si = star_s;
    } else {
      return false;
    }
  }
  if (si > s.size()) return false;  // un '_' saltó un carácter UTF-8 truncado
  while (pi < p.size() && p[pi] == '%') ++pi;
  return pi == p.size();
}

uint32_t Program::add_const(const Value& v) {
  Value c = v;
  if (v.type == VType::Str || v.type == VType::Json) {
    strings.push_back(std::make_unique<std::string>(v.str()));
    const std::string& stored = *strings.back();
    c.s = stored.data();
    c.len = static_cast<uint32_t>(stored.size());
  }
  // Reutilizar constantes idénticas.
  for (uint32_t i = 0; i < consts.size(); ++i) {
    const Value& e = consts[i];
    if (e.type != c.type) continue;
    if ((c.type == VType::Num || c.type == VType::Bool) && (e.num == c.num || (e.num != e.num && c.num != c.num)))
      return i;
    if (c.type == VType::Null) return i;
    if ((c.type == VType::Str || c.type == VType::Json) && e.str() == c.str()) {
      if (!strings.empty() && strings.back()->data() == c.s) strings.pop_back();
      return i;
    }
  }
  consts.push_back(c);
  return static_cast<uint32_t>(consts.size() - 1);
}

std::string Program::disassemble() const {
  std::string out;
  for (size_t i = 0; i < code.size(); ++i) {
    const Instr& in = code[i];
    if (i < 10) out += ' ';
    if (i < 100) out += ' ';
    append_uint(out, i);
    out += "  ";
    out += opcode_name(in.op);
    switch (in.op) {
      case OpCode::Const:
        out += ' ';
        if (in.a < consts.size()) {
          const Value& v = consts[in.a];
          if (v.type == VType::Str) {
            out += '\'';
            out.append(v.str());
            out += '\'';
          } else {
            append_text(out, v);
          }
        }
        break;
      case OpCode::Field:
      case OpCode::Key:
      case OpCode::Agg:
      case OpCode::Col:
      case OpCode::InSet:
      case OpCode::Like:
        out += " #";
        append_uint(out, in.a);
        break;
      case OpCode::JumpIfFalse:
      case OpCode::JumpIfTrue:
      case OpCode::JumpIfNotTrue:
      case OpCode::Jump:
        out += " -> ";
        append_uint(out, in.a);
        break;
      case OpCode::Call:
        out += " fn#";
        append_uint(out, in.a);
        out += " argc=";
        append_uint(out, in.b);
        break;
      default: break;
    }
    out += '\n';
  }
  return out;
}

}  // namespace zr::zql
