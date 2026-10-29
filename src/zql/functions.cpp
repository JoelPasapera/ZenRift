#include "zql/functions.hpp"

#include <cmath>
#include <cstring>

#include "core/strings.hpp"
#include "core/time.hpp"

namespace zr::zql {

namespace {

bool num_arg(const Value& v, double& out) { return to_number(v, out); }

bool str_arg(const Value& v, std::string_view& out) {
  if (v.type != VType::Str) return false;
  out = v.str();
  return true;
}

Value make_str(Arena& a, std::string_view s) { return Value::string(a.copy(s)); }

Value unary_math(const Value* a, double (*f)(double)) {
  double x;
  if (!num_arg(a[0], x)) return Value::null();
  const double r = f(x);
  return std::isfinite(r) ? Value::number(r) : Value::null();
}

// UTF-8: avanza un carácter.
size_t next_char(std::string_view s, size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  return std::min(s.size(), i + n);
}

Value fn_lower(const Value* a, uint32_t, Arena& ar) {
  std::string_view s;
  if (!str_arg(a[0], s)) return Value::null();
  char* p = static_cast<char*>(ar.alloc(s.size() + 1, 1));
  for (size_t i = 0; i < s.size(); ++i) p[i] = (s[i] >= 'A' && s[i] <= 'Z') ? static_cast<char>(s[i] + 32) : s[i];
  return Value::string({p, s.size()});
}

Value fn_upper(const Value* a, uint32_t, Arena& ar) {
  std::string_view s;
  if (!str_arg(a[0], s)) return Value::null();
  char* p = static_cast<char*>(ar.alloc(s.size() + 1, 1));
  for (size_t i = 0; i < s.size(); ++i) p[i] = (s[i] >= 'a' && s[i] <= 'z') ? static_cast<char>(s[i] - 32) : s[i];
  return Value::string({p, s.size()});
}

Value fn_length(const Value* a, uint32_t, Arena&) {
  std::string_view s;
  if (!str_arg(a[0], s)) return Value::null();
  double n = 0;
  for (char c : s)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  return Value::number(n);
}

Value fn_trim(const Value* a, uint32_t, Arena&) {
  std::string_view s;
  if (!str_arg(a[0], s)) return Value::null();
  return Value::string(trim(s));
}

Value fn_abs(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::fabs(x); }); }
Value fn_floor(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::floor(x); }); }
Value fn_ceil(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::ceil(x); }); }
Value fn_sqrt(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::sqrt(x); }); }
Value fn_ln(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::log(x); }); }
Value fn_log10(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::log10(x); }); }
Value fn_log2(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::log2(x); }); }
Value fn_exp(const Value* a, uint32_t, Arena&) { return unary_math(a, [](double x) { return std::exp(x); }); }
Value fn_sign(const Value* a, uint32_t, Arena&) {
  return unary_math(a, [](double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); });
}

Value fn_round(const Value* a, uint32_t argc, Arena&) {
  double x, d = 0;
  if (!num_arg(a[0], x)) return Value::null();
  if (argc > 1 && !num_arg(a[1], d)) return Value::null();
  if (d == 0) return Value::number(std::round(x));
  const double f = std::pow(10.0, std::floor(d));
  const double r = std::round(x * f) / f;
  return std::isfinite(r) ? Value::number(r) : Value::number(x);
}

Value fn_pow(const Value* a, uint32_t, Arena&) {
  double x, y;
  if (!num_arg(a[0], x) || !num_arg(a[1], y)) return Value::null();
  const double r = std::pow(x, y);
  return std::isfinite(r) ? Value::number(r) : Value::null();
}

Value fn_least(const Value* a, uint32_t argc, Arena&) {
  bool any = false;
  double best = 0;
  for (uint32_t i = 0; i < argc; ++i) {
    double x;
    if (num_arg(a[i], x) && (!any || x < best)) {
      best = x;
      any = true;
    }
  }
  return any ? Value::number(best) : Value::null();
}

Value fn_greatest(const Value* a, uint32_t argc, Arena&) {
  bool any = false;
  double best = 0;
  for (uint32_t i = 0; i < argc; ++i) {
    double x;
    if (num_arg(a[i], x) && (!any || x > best)) {
      best = x;
      any = true;
    }
  }
  return any ? Value::number(best) : Value::null();
}

Value fn_coalesce(const Value* a, uint32_t argc, Arena&) {
  for (uint32_t i = 0; i < argc; ++i)
    if (!a[i].is_null()) return a[i];
  return Value::null();
}

Value fn_if(const Value* a, uint32_t, Arena&) { return truth(a[0]) == Tri::True ? a[1] : a[2]; }

Value fn_nullif(const Value* a, uint32_t, Arena&) { return equals(a[0], a[1]) == Tri::True ? Value::null() : a[0]; }

Value fn_contains(const Value* a, uint32_t, Arena&) {
  std::string_view s, sub;
  if (!str_arg(a[0], s) || !str_arg(a[1], sub)) return Value::null();
  return Value::boolean(s.find(sub) != std::string_view::npos);
}

Value fn_starts_with(const Value* a, uint32_t, Arena&) {
  std::string_view s, p;
  if (!str_arg(a[0], s) || !str_arg(a[1], p)) return Value::null();
  return Value::boolean(s.starts_with(p));
}

Value fn_ends_with(const Value* a, uint32_t, Arena&) {
  std::string_view s, p;
  if (!str_arg(a[0], s) || !str_arg(a[1], p)) return Value::null();
  return Value::boolean(s.ends_with(p));
}

// substr(s, inicio [, longitud]) con índices desde 1 y en caracteres UTF-8.
Value fn_substr(const Value* a, uint32_t argc, Arena&) {
  std::string_view s;
  double start, len = 1e18;
  if (!str_arg(a[0], s) || !num_arg(a[1], start)) return Value::null();
  if (argc > 2 && !num_arg(a[2], len)) return Value::null();
  if (len <= 0) return Value::string({});
  if (start < 1) {
    len += start - 1;
    start = 1;
    if (len <= 0) return Value::string({});
  }
  size_t i = 0;
  for (double k = 1; k < start && i < s.size(); ++k) i = next_char(s, i);
  size_t j = i;
  for (double k = 0; k < len && j < s.size(); ++k) j = next_char(s, j);
  return Value::string(s.substr(i, j - i));
}

Value fn_split_part(const Value* a, uint32_t, Arena&) {
  std::string_view s, sep;
  double n;
  if (!str_arg(a[0], s) || !str_arg(a[1], sep) || !num_arg(a[2], n) || sep.empty() || n < 1) return Value::null();
  size_t start = 0;
  for (double k = 1; k < n; ++k) {
    const size_t p = s.find(sep, start);
    if (p == std::string_view::npos) return Value::string({});
    start = p + sep.size();
  }
  const size_t end = s.find(sep, start);
  return Value::string(s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

Value fn_replace(const Value* a, uint32_t, Arena& ar) {
  std::string_view s, from, to;
  if (!str_arg(a[0], s) || !str_arg(a[1], from) || !str_arg(a[2], to)) return Value::null();
  if (from.empty()) return a[0];
  std::string out;
  size_t pos = 0;
  while (true) {
    const size_t p = s.find(from, pos);
    if (p == std::string_view::npos) break;
    out.append(s.substr(pos, p - pos));
    out.append(to);
    pos = p + from.size();
  }
  out.append(s.substr(pos));
  return make_str(ar, out);
}

Value fn_concat(const Value* a, uint32_t argc, Arena& ar) {
  std::string out;
  for (uint32_t i = 0; i < argc; ++i)
    if (!a[i].is_null()) append_text(out, a[i]);
  return make_str(ar, out);
}

Value fn_str(const Value* a, uint32_t, Arena& ar) {
  if (a[0].is_null()) return Value::null();
  if (a[0].type == VType::Str) return a[0];
  std::string out;
  append_text(out, a[0]);
  return make_str(ar, out);
}

Value fn_num(const Value* a, uint32_t, Arena&) {
  double x;
  if (to_number(a[0], x)) return Value::number(x);
  if (a[0].type == VType::Str && parse_double(trim(a[0].str()), x)) return Value::number(x);
  return Value::null();
}

Value fn_bucket(const Value* a, uint32_t, Arena&) {
  double x, w;
  if (!num_arg(a[0], x) || !num_arg(a[1], w) || !(w > 0)) return Value::null();
  return Value::number(std::floor(x / w) * w);
}

Value fn_clamp(const Value* a, uint32_t, Arena&) {
  double x, lo, hi;
  if (!num_arg(a[0], x) || !num_arg(a[1], lo) || !num_arg(a[2], hi)) return Value::null();
  return Value::number(x < lo ? lo : (x > hi ? hi : x));
}

Value fn_now(const Value*, uint32_t, Arena&) { return Value::number(static_cast<double>(wall_ms())); }

constexpr FunctionInfo kFunctions[] = {
    {"lower", 1, 1, true, fn_lower, "lower(s): minúsculas (ASCII)"},
    {"upper", 1, 1, true, fn_upper, "upper(s): mayúsculas (ASCII)"},
    {"length", 1, 1, true, fn_length, "length(s): número de caracteres"},
    {"len", 1, 1, true, fn_length, "len(s): alias de length"},
    {"trim", 1, 1, true, fn_trim, "trim(s): quita espacios de los extremos"},
    {"abs", 1, 1, true, fn_abs, "abs(x)"},
    {"floor", 1, 1, true, fn_floor, "floor(x)"},
    {"ceil", 1, 1, true, fn_ceil, "ceil(x)"},
    {"round", 1, 2, true, fn_round, "round(x [, decimales])"},
    {"sqrt", 1, 1, true, fn_sqrt, "sqrt(x)"},
    {"ln", 1, 1, true, fn_ln, "ln(x): logaritmo natural"},
    {"log10", 1, 1, true, fn_log10, "log10(x)"},
    {"log2", 1, 1, true, fn_log2, "log2(x)"},
    {"exp", 1, 1, true, fn_exp, "exp(x)"},
    {"pow", 2, 2, true, fn_pow, "pow(x, y)"},
    {"power", 2, 2, true, fn_pow, "power(x, y): alias de pow"},
    {"sign", 1, 1, true, fn_sign, "sign(x): -1, 0 o 1"},
    {"least", 1, 16, true, fn_least, "least(a, b, ...): el menor número"},
    {"greatest", 1, 16, true, fn_greatest, "greatest(a, b, ...): el mayor número"},
    {"coalesce", 1, 16, true, fn_coalesce, "coalesce(a, b, ...): el primer valor no nulo"},
    {"if", 3, 3, true, fn_if, "if(cond, a, b)"},
    {"nullif", 2, 2, true, fn_nullif, "nullif(a, b): null si a = b"},
    {"contains", 2, 2, true, fn_contains, "contains(s, sub)"},
    {"starts_with", 2, 2, true, fn_starts_with, "starts_with(s, prefijo)"},
    {"ends_with", 2, 2, true, fn_ends_with, "ends_with(s, sufijo)"},
    {"substr", 2, 3, true, fn_substr, "substr(s, inicio [, longitud]): desde 1, en caracteres"},
    {"split_part", 3, 3, true, fn_split_part, "split_part(s, separador, n): n-ésimo trozo, desde 1"},
    {"replace", 3, 3, true, fn_replace, "replace(s, buscar, reemplazo)"},
    {"concat", 1, 16, true, fn_concat, "concat(a, b, ...): concatena ignorando nulos"},
    {"str", 1, 1, true, fn_str, "str(x): a texto"},
    {"num", 1, 1, true, fn_num, "num(x): a número (acepta texto numérico)"},
    {"bucket", 2, 2, true, fn_bucket, "bucket(x, ancho): floor(x / ancho) * ancho"},
    {"clamp", 3, 3, true, fn_clamp, "clamp(x, min, max)"},
    {"now", 0, 0, false, fn_now, "now(): hora actual en ms desde la época"},
};

constexpr size_t kFunctionCount = sizeof(kFunctions) / sizeof(kFunctions[0]);

}  // namespace

const FunctionInfo* find_function(std::string_view name) noexcept {
  for (const auto& f : kFunctions)
    if (name == f.name) return &f;
  return nullptr;
}

uint32_t function_id(const FunctionInfo* f) noexcept { return static_cast<uint32_t>(f - kFunctions); }

const FunctionInfo& function_at(uint32_t id) noexcept { return kFunctions[id]; }

size_t function_count() noexcept { return kFunctionCount; }

}  // namespace zr::zql
