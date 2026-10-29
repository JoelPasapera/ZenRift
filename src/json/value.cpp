#include "json/value.hpp"

#include "core/strings.hpp"
#include "json/lex.hpp"

namespace zr::json {

Value Value::boolean(bool b) {
  Value v;
  v.type_ = Type::Bool;
  v.bool_ = b;
  return v;
}

Value Value::number(double n) {
  Value v;
  v.type_ = Type::Number;
  v.num_ = n;
  return v;
}

Value Value::string(std::string s) {
  Value v;
  v.type_ = Type::String;
  v.str_ = std::move(s);
  return v;
}

Value Value::array() {
  Value v;
  v.type_ = Type::Array;
  return v;
}

Value Value::object() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

const std::string& Value::as_string() const noexcept {
  static const std::string kEmpty;
  return type_ == Type::String ? str_ : kEmpty;
}

const Value* Value::get(std::string_view key) const noexcept {
  if (type_ != Type::Object) return nullptr;
  for (const auto& m : obj_)
    if (m.first == key) return &m.second;
  return nullptr;
}

std::string_view Value::get_string(std::string_view key, std::string_view def) const noexcept {
  const Value* v = get(key);
  return v && v->is_string() ? std::string_view(v->str_) : def;
}

double Value::get_number(std::string_view key, double def) const noexcept {
  const Value* v = get(key);
  return v && v->is_number() ? v->num_ : def;
}

bool Value::get_bool(std::string_view key, bool def) const noexcept {
  const Value* v = get(key);
  return v && v->is_bool() ? v->bool_ : def;
}

Value& Value::set(std::string key, Value v) {
  for (auto& m : obj_) {
    if (m.first == key) {
      m.second = std::move(v);
      return m.second;
    }
  }
  obj_.emplace_back(std::move(key), std::move(v));
  return obj_.back().second;
}

void Value::push(Value v) { arr_.push_back(std::move(v)); }

namespace {

class Parser {
 public:
  Parser(std::string_view text, ParseError* err) : begin_(text.data()), p_(text.data()), end_(text.data() + text.size()), err_(err) {}

  bool document(Value& out) {
    if (!value(out, 0)) return false;
    p_ = lex::skip_ws(p_, end_);
    if (p_ != end_) return fail("contenido inesperado tras el documento JSON");
    return true;
  }

 private:
  bool fail(const char* msg) {
    if (err_) {
      err_->offset = static_cast<size_t>(p_ - begin_);
      err_->message = msg;
    }
    return false;
  }

  bool value(Value& out, int depth) {
    if (depth > lex::kMaxDepth) return fail("anidamiento demasiado profundo");
    p_ = lex::skip_ws(p_, end_);
    if (p_ >= end_) return fail("fin inesperado del JSON");
    switch (*p_) {
      case '{': return object(out, depth);
      case '[': return array(out, depth);
      case '"': {
        std::string s;
        if (!string(s)) return false;
        out = Value::string(std::move(s));
        return true;
      }
      case 't':
        if (!lex::match_literal(p_, end_, "true")) return fail("literal inválido");
        out = Value::boolean(true);
        return true;
      case 'f':
        if (!lex::match_literal(p_, end_, "false")) return fail("literal inválido");
        out = Value::boolean(false);
        return true;
      case 'n':
        if (!lex::match_literal(p_, end_, "null")) return fail("literal inválido");
        out = Value();
        return true;
      default: {
        double d;
        if (!lex::scan_number(p_, end_, d)) return fail("número inválido");
        out = Value::number(d);
        return true;
      }
    }
  }

  bool string(std::string& out) {
    std::string_view raw;
    bool esc;
    if (!lex::scan_string(p_, end_, raw, esc)) return fail("cadena inválida");
    if (esc)
      lex::unescape(raw, out);
    else
      out.assign(raw);
    return true;
  }

  bool object(Value& out, int depth) {
    out = Value::object();
    ++p_;
    p_ = lex::skip_ws(p_, end_);
    if (p_ < end_ && *p_ == '}') {
      ++p_;
      return true;
    }
    while (true) {
      p_ = lex::skip_ws(p_, end_);
      if (p_ >= end_ || *p_ != '"') return fail("se esperaba una clave entre comillas");
      std::string key;
      if (!string(key)) return false;
      p_ = lex::skip_ws(p_, end_);
      if (p_ >= end_ || *p_ != ':') return fail("se esperaba ':'");
      ++p_;
      Value v;
      if (!value(v, depth + 1)) return false;
      out.set(std::move(key), std::move(v));
      p_ = lex::skip_ws(p_, end_);
      if (p_ >= end_) return fail("objeto sin cerrar");
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == '}') {
        ++p_;
        return true;
      }
      return fail("se esperaba ',' o '}'");
    }
  }

  bool array(Value& out, int depth) {
    out = Value::array();
    ++p_;
    p_ = lex::skip_ws(p_, end_);
    if (p_ < end_ && *p_ == ']') {
      ++p_;
      return true;
    }
    while (true) {
      Value v;
      if (!value(v, depth + 1)) return false;
      out.push(std::move(v));
      p_ = lex::skip_ws(p_, end_);
      if (p_ >= end_) return fail("array sin cerrar");
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == ']') {
        ++p_;
        return true;
      }
      return fail("se esperaba ',' o ']'");
    }
  }

  const char* begin_;
  const char* p_;
  const char* end_;
  ParseError* err_;
};

}  // namespace

bool parse(std::string_view text, Value& out, ParseError* err) {
  Parser p(text, err);
  return p.document(out);
}

void write(std::string& out, const Value& v) {
  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Value::Type::Number: append_double(out, v.as_number()); break;
    case Value::Type::String:
      out.push_back('"');
      append_json_escaped(out, v.as_string());
      out.push_back('"');
      break;
    case Value::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : v.items()) {
        if (!first) out.push_back(',');
        first = false;
        write(out, item);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& m : v.members()) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        append_json_escaped(out, m.first);
        out += "\":";
        write(out, m.second);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string to_string(const Value& v) {
  std::string s;
  write(s, v);
  return s;
}

}  // namespace zr::json
