// Valor de ejecución de ZQL: 24 bytes, sin propiedad de memoria.
//
// Las cadenas son vistas (puntero + longitud) a memoria que vive más que la
// evaluación: el lote de eventos, el arena de trabajo o el pool de constantes
// del programa. Los booleanos se guardan en `num` (0/1).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zr::zql {

enum class VType : uint8_t { Null, Bool, Num, Str, Json };

struct Value {
  const char* s = nullptr;
  double num = 0;
  uint32_t len = 0;
  VType type = VType::Null;

  static Value null() noexcept { return {}; }
  static Value boolean(bool b) noexcept {
    Value v;
    v.type = VType::Bool;
    v.num = b ? 1.0 : 0.0;
    return v;
  }
  static Value number(double n) noexcept {
    Value v;
    v.type = VType::Num;
    v.num = n;
    return v;
  }
  static Value string(std::string_view sv) noexcept {
    Value v;
    v.type = VType::Str;
    v.s = sv.data();
    v.len = static_cast<uint32_t>(sv.size());
    return v;
  }
  // Fragmento de JSON ya serializado (objetos/arrays de los eventos, top-k).
  static Value json(std::string_view sv) noexcept {
    Value v = string(sv);
    v.type = VType::Json;
    return v;
  }

  bool is_null() const noexcept { return type == VType::Null; }
  std::string_view str() const noexcept { return {s, len}; }
};

// Lógica trivalente de SQL.
enum class Tri : uint8_t { False, True, Unknown };

inline Tri truth(const Value& v) noexcept {
  switch (v.type) {
    case VType::Bool: return v.num != 0 ? Tri::True : Tri::False;
    case VType::Num: return v.num != v.num ? Tri::Unknown : (v.num != 0 ? Tri::True : Tri::False);
    default: return Tri::Unknown;
  }
}

// Coerción numérica: números tal cual, booleanos como 0/1. Es lo que permite
// escribir avg(status >= 500) para obtener una tasa de error.
inline bool to_number(const Value& v, double& out) noexcept {
  if (v.type == VType::Num || v.type == VType::Bool) {
    out = v.num;
    return true;
  }
  return false;
}

const char* type_name(VType t) noexcept;

// Codificación binaria canónica (claves de grupo, top-k, conjuntos IN):
// etiqueta de tipo + carga. -0 y 0 codifican igual.
void encode_value(const Value& v, std::string& out);
// Decodifica un valor; las cadenas resultantes apuntan dentro de `bytes`.
bool decode_value(std::string_view bytes, size_t& pos, Value& out) noexcept;

// JSON del valor (Str escapado, Json tal cual, NaN/Inf como null).
void write_json(std::string& out, const Value& v);
// Texto para concatenaciones y str(): sin comillas.
void append_text(std::string& out, const Value& v);

// Igualdad de SQL entre valores no nulos del mismo tipo (números por valor,
// cadenas por bytes). Devuelve Unknown si alguno es null o los tipos difieren
// de forma incomparable.
Tri equals(const Value& a, const Value& b) noexcept;
// Comparación de orden: <0, 0, >0. `ok` = false si no son comparables.
int compare(const Value& a, const Value& b, bool& ok) noexcept;
// Orden total para ORDER BY: null < bool < número < cadena < json.
int total_order(const Value& a, const Value& b) noexcept;

}  // namespace zr::zql
