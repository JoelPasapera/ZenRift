// Escritor JSON en flujo: añade texto compacto a un std::string sin construir
// un DOM. Lleva la cuenta de las comas con una pila de bits (64 niveles).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/strings.hpp"

namespace zr::json {

class Writer {
 public:
  explicit Writer(std::string& out) : out_(out) {}

  Writer& begin_object() {
    sep();
    out_.push_back('{');
    push();
    return *this;
  }
  Writer& end_object() {
    out_.push_back('}');
    pop();
    return *this;
  }
  Writer& begin_array() {
    sep();
    out_.push_back('[');
    push();
    return *this;
  }
  Writer& end_array() {
    out_.push_back(']');
    pop();
    return *this;
  }

  Writer& key(std::string_view k) {
    sep();
    out_.push_back('"');
    append_json_escaped(out_, k);
    out_.append("\":");
    after_key_ = true;
    return *this;
  }

  Writer& str(std::string_view s) {
    sep();
    out_.push_back('"');
    append_json_escaped(out_, s);
    out_.push_back('"');
    return *this;
  }
  Writer& num(double v) {
    sep();
    append_double(out_, v);
    return *this;
  }
  Writer& integer(int64_t v) {
    sep();
    append_int(out_, v);
    return *this;
  }
  Writer& boolean(bool b) {
    sep();
    out_.append(b ? "true" : "false");
    return *this;
  }
  Writer& null() {
    sep();
    out_.append("null");
    return *this;
  }
  // Inserta JSON ya serializado (se confía en que es válido).
  Writer& raw(std::string_view json) {
    sep();
    out_.append(json);
    return *this;
  }

  // Atajos clave/valor.
  Writer& kv(std::string_view k, std::string_view v) { return key(k).str(v); }
  Writer& kv(std::string_view k, const char* v) { return key(k).str(v); }
  Writer& kv_num(std::string_view k, double v) { return key(k).num(v); }
  Writer& kv_int(std::string_view k, int64_t v) { return key(k).integer(v); }
  Writer& kv_bool(std::string_view k, bool v) { return key(k).boolean(v); }

  std::string& out() noexcept { return out_; }

 private:
  void sep() {
    if (after_key_) {
      after_key_ = false;
      return;
    }
    if (depth_ == 0) return;
    const uint64_t bit = uint64_t{1} << ((depth_ - 1) & 63);
    if (first_ & bit)
      first_ &= ~bit;
    else
      out_.push_back(',');
  }
  void push() {
    ++depth_;
    first_ |= uint64_t{1} << ((depth_ - 1) & 63);
  }
  void pop() { --depth_; }

  std::string& out_;
  uint64_t first_ = 0;
  int depth_ = 0;
  bool after_key_ = false;
};

}  // namespace zr::json
