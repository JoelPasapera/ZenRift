// Árbol sintáctico de ZQL.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zql/diagnostic.hpp"

namespace zr::zql {

enum class ExprKind : uint8_t {
  Null,
  Bool,
  Number,
  String,
  Field,    // ruta de campo: a.b.c
  Call,     // función escalar o agregación
  Star,     // '*' dentro de count(*)
  Unary,    // -x, NOT x
  Binary,   // aritmética, comparación, lógica, ||
  In,       // x [NOT] IN (a, b, ...)
  Between,  // x [NOT] BETWEEN a AND b
  Like,     // x [NOT] LIKE 'patrón'   /   ILIKE
  IsNull,   // x IS [NOT] NULL
  Case,     // CASE WHEN c THEN v ... [ELSE e] END
};

enum class Op : uint8_t {
  None,
  Neg,
  Not,
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Concat,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  And,
  Or,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::Null;
  Op op = Op::None;
  bool bool_value = false;
  double number = 0;
  std::string text;          // cadena literal, ruta de campo o nombre de función (en minúsculas)
  std::vector<ExprPtr> args; // hijos; en Case: [c1, v1, c2, v2, ..., (else)]
  bool negated = false;      // NOT IN, NOT BETWEEN, NOT LIKE, IS NOT NULL
  bool case_insensitive = false;  // ILIKE
  bool has_else = false;          // Case
  SourcePos pos;
  size_t end_offset = 0;     // fin del texto de la expresión (para nombres de columna)
};

struct SelectItem {
  ExprPtr expr;
  std::string alias;  // vacío si no hay AS
  SourcePos pos;
  std::string source; // texto original de la expresión
};

struct OrderItem {
  ExprPtr expr;
  bool desc = false;
};

enum class WindowType : uint8_t { Tumbling, Hopping };

struct WindowClause {
  bool present = false;
  WindowType type = WindowType::Tumbling;
  int64_t size_ms = 0;
  int64_t advance_ms = 0;
  int64_t lateness_ms = -1;  // -1: usar el valor por defecto
  SourcePos pos;
};

struct QueryAst {
  std::vector<SelectItem> select;
  std::string from;
  SourcePos from_pos;
  ExprPtr where;
  std::vector<ExprPtr> group_by;
  WindowClause window;
  ExprPtr having;
  int64_t emit_every_ms = 0;
  SourcePos emit_pos;
  ExprPtr alert;
  std::string alert_source;
  uint32_t alert_for = 1;
  std::vector<OrderItem> order_by;
  int64_t limit = -1;
  SourcePos limit_pos;
};

// Igualdad estructural (para reconocer expresiones del GROUP BY en el SELECT).
bool same_expr(const Expr& a, const Expr& b) noexcept;

// Forma canónica en texto (para deduplicar agregaciones y nombrar columnas).
std::string canonical(const Expr& e);

}  // namespace zr::zql
