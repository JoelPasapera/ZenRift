// Documento JSON en memoria (DOM pequeño) para cuerpos de la API, el catálogo
// y la configuración. La ingesta de eventos NO pasa por aquí: usa el escáner
// de la ruta caliente (engine/event_scanner).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zr::json {

class Value {
 public:
  enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
  using Member = std::pair<std::string, Value>;

  Value() = default;
  static Value boolean(bool b);
  static Value number(double n);
  static Value string(std::string s);
  static Value array();
  static Value object();

  Type type() const noexcept { return type_; }
  bool is_null() const noexcept { return type_ == Type::Null; }
  bool is_bool() const noexcept { return type_ == Type::Bool; }
  bool is_number() const noexcept { return type_ == Type::Number; }
  bool is_string() const noexcept { return type_ == Type::String; }
  bool is_array() const noexcept { return type_ == Type::Array; }
  bool is_object() const noexcept { return type_ == Type::Object; }

  bool as_bool(bool def = false) const noexcept { return type_ == Type::Bool ? bool_ : def; }
  double as_number(double def = 0) const noexcept { return type_ == Type::Number ? num_ : def; }
  const std::string& as_string() const noexcept;

  const std::vector<Value>& items() const noexcept { return arr_; }
  std::vector<Value>& items() noexcept { return arr_; }
  const std::vector<Member>& members() const noexcept { return obj_; }

  // Búsqueda en objetos (lineal: los objetos de la API son pequeños).
  const Value* get(std::string_view key) const noexcept;
  std::string_view get_string(std::string_view key, std::string_view def = {}) const noexcept;
  double get_number(std::string_view key, double def) const noexcept;
  bool get_bool(std::string_view key, bool def) const noexcept;

  Value& set(std::string key, Value v);
  void push(Value v);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<Member> obj_;
};

struct ParseError {
  size_t offset = 0;
  std::string message;
};

// Analiza un documento completo (sin basura al final).
bool parse(std::string_view text, Value& out, ParseError* err = nullptr);

// Serialización compacta.
void write(std::string& out, const Value& v);
std::string to_string(const Value& v);

}  // namespace zr::json
