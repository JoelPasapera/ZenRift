// Parser de ZQL: descenso recursivo para las cláusulas y precedencia por
// niveles para las expresiones.
//
//   SELECT <expr> [AS alias], ...
//   FROM <stream>
//   [WHERE <expr>]
//   [GROUP BY <expr>, ...]
//   [WINDOW TUMBLING <dur> | WINDOW HOPPING <dur> EVERY <dur>] [LATENESS <dur>]
//   [HAVING <expr>]
//   [ORDER BY <expr> [ASC|DESC], ...] [LIMIT <n>]
//   [EMIT EVERY <dur>]
//   [ALERT IF <expr> [FOR <n> WINDOWS]]
//
// Las cláusulas opcionales pueden ir en cualquier orden, cada una una vez.
#pragma once

#include <string_view>

#include "zql/ast.hpp"
#include "zql/diagnostic.hpp"

namespace zr::zql {

bool parse_query(std::string_view src, QueryAst& out, Diagnostic& err);

// Una expresión suelta (filtros del tail en vivo).
bool parse_expression(std::string_view src, ExprPtr& out, Diagnostic& err);

}  // namespace zr::zql
