// Plan compilado de una consulta continua: todo lo que el motor necesita para
// ejecutarla, ya resuelto (programas de bytecode, agregaciones deduplicadas,
// ventana, orden, alerta). Es inmutable y se comparte entre hilos con
// shared_ptr<const QueryPlan>.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zql/ast.hpp"
#include "zql/diagnostic.hpp"
#include "zql/program.hpp"

namespace zr::zql {

enum class AggKind : uint8_t {
  Count,        // count(*)
  CountValues,  // count(x): valores no nulos
  Sum,
  Avg,
  Min,
  Max,
  Stddev,
  Variance,
  Quantile,     // p50..p999, median, quantile(x, q)  -> DDSketch
  Distinct,     // distinct(x), count(DISTINCT x)     -> HyperLogLog
  TopK,         // topk(x, k)                         -> Space-Saving
  First,        // primer valor por tiempo de evento
  Last,         // último valor por tiempo de evento
  Rate,         // rate(): eventos/s; rate(x): suma de x por segundo
};

const char* agg_kind_name(AggKind k) noexcept;

struct AggSpec {
  AggKind kind = AggKind::Count;
  bool has_arg = false;
  Program arg;         // ámbito de evento
  double q = 0;        // cuantil
  uint32_t k = 0;      // top-k
  std::string signature;
};

struct OutputColumn {
  std::string name;
  Program prog;        // ámbito de fila
};

struct OrderKey {
  Program prog;
  bool desc = false;
  std::string text;
};

struct QueryPlan {
  std::string name;
  std::string text;
  std::string stream;
  std::vector<std::string> fields;  // rutas que leen los programas de ámbito de evento

  bool has_where = false;
  Program where;
  std::string where_text;

  std::vector<Program> keys;
  std::vector<std::string> key_names;

  std::vector<AggSpec> aggs;
  std::vector<OutputColumn> columns;

  bool has_having = false;
  Program having;
  std::string having_text;

  bool has_alert = false;
  Program alert;
  std::string alert_text;
  uint32_t alert_for = 1;

  std::vector<OrderKey> order;
  int64_t limit = -1;

  WindowType window_type = WindowType::Tumbling;
  int64_t window_ms = 0;
  int64_t advance_ms = 0;  // tamaño del panel
  int64_t lateness_ms = 0;
  int64_t emit_every_ms = 0;

  QueryPlan() = default;
  QueryPlan(QueryPlan&&) noexcept = default;
  QueryPlan& operator=(QueryPlan&&) noexcept = default;

  bool global() const noexcept { return keys.empty(); }
  uint32_t panes_per_window() const noexcept { return static_cast<uint32_t>(window_ms / advance_ms); }
  // Explicación legible del plan (para `zenrift check` y el panel).
  std::string explain(bool with_bytecode = false) const;
};

// Filtro suelto (tail en vivo): una expresión de ámbito de evento.
struct FilterPlan {
  std::string text;
  std::vector<std::string> fields;
  Program prog;
};

struct CompileOptions {
  int64_t default_window_ms = 10000;
  int64_t default_lateness_ms = 2000;
  int64_t min_window_ms = 100;
  int64_t max_window_ms = 7LL * 86400000;
  int64_t max_lateness_ms = 3600000;
  uint32_t max_panes_per_window = 720;
  int64_t min_emit_ms = 100;
};

bool compile_query(std::string_view text, const CompileOptions& opts, QueryPlan& out, Diagnostic& err);
bool compile_filter(std::string_view text, FilterPlan& out, Diagnostic& err);

}  // namespace zr::zql
