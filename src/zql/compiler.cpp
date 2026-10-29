// Compilador de ZQL: valida la consulta y traduce cada expresión a bytecode.
#include <algorithm>
#include <cmath>

#include "core/arena.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "zql/functions.hpp"
#include "zql/parser.hpp"
#include "zql/plan.hpp"
#include "zql/vm.hpp"

namespace zr::zql {

const char* agg_kind_name(AggKind k) noexcept {
  switch (k) {
    case AggKind::Count: return "count";
    case AggKind::CountValues: return "count";
    case AggKind::Sum: return "sum";
    case AggKind::Avg: return "avg";
    case AggKind::Min: return "min";
    case AggKind::Max: return "max";
    case AggKind::Stddev: return "stddev";
    case AggKind::Variance: return "variance";
    case AggKind::Quantile: return "quantile";
    case AggKind::Distinct: return "distinct";
    case AggKind::TopK: return "topk";
    case AggKind::First: return "first";
    case AggKind::Last: return "last";
    case AggKind::Rate: return "rate";
  }
  return "?";
}

namespace {

struct CompileFail {};

enum class Scope { Event, Row };

// Emisor con seguimiento de la profundidad de la pila.
struct Emit {
  Program* p;
  uint32_t depth = 0;

  void op(OpCode o, uint32_t a = 0, uint8_t b = 0) { p->code.push_back(Instr{o, b, 0, a}); }
  void push(uint32_t n = 1) {
    depth += n;
    if (depth > p->max_stack) p->max_stack = depth;
  }
  void pop(uint32_t n = 1) { depth -= n; }
  size_t here() const { return p->code.size(); }
  void patch_to_here(size_t at) { p->code[at].a = static_cast<uint32_t>(p->code.size()); }
};

struct Ctx {
  Scope scope = Scope::Event;
  const char* clause = "";
  bool allow_aliases = false;
  bool in_aggregate = false;
};

// ¿Es un nombre de agregación? Rellena tipo y parámetro implícito.
bool aggregate_info(std::string_view name, AggKind& kind, double& q) {
  q = 0;
  if (name == "count") kind = AggKind::Count;
  else if (name == "sum") kind = AggKind::Sum;
  else if (name == "avg" || name == "mean") kind = AggKind::Avg;
  else if (name == "min") kind = AggKind::Min;
  else if (name == "max") kind = AggKind::Max;
  else if (name == "stddev" || name == "stddev_samp") kind = AggKind::Stddev;
  else if (name == "variance" || name == "var_samp") kind = AggKind::Variance;
  else if (name == "median") {
    kind = AggKind::Quantile;
    q = 0.5;
  } else if (name == "quantile" || name == "percentile") kind = AggKind::Quantile;
  else if (name == "distinct" || name == "count_distinct" || name == "approx_distinct" || name == "ndv")
    kind = AggKind::Distinct;
  else if (name == "topk" || name == "top") kind = AggKind::TopK;
  else if (name == "first") kind = AggKind::First;
  else if (name == "last") kind = AggKind::Last;
  else if (name == "rate") kind = AggKind::Rate;
  else if (name.size() >= 2 && name.size() <= 5 && name[0] == 'p' &&
           std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    // p50 -> 0.5, p99 -> 0.99, p999 -> 0.999, p5 -> 0.05, p100 -> 1
    const std::string_view d = name.substr(1);
    kind = AggKind::Quantile;
    if (d == "100") q = 1.0;
    else if (d.size() == 1) q = (d[0] - '0') / 100.0;
    else {
      // Entero / 10^dígitos: una sola división correctamente redondeada, así
      // p95 da exactamente el double más cercano a 0.95 (sumar 0.9 + 0.05
      // daría 0.9500000000000001).
      double digits = 0, denom = 1;
      for (char c : d) {
        digits = digits * 10 + (c - '0');
        denom *= 10;
      }
      q = digits / denom;
    }
  } else
    return false;
  return true;
}

bool is_window_function(std::string_view name) {
  return name == "window_start" || name == "window_end" || name == "window_seconds";
}

size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
    prev.swap(cur);
  }
  return prev[b.size()];
}

std::string suggest_function(std::string_view name) {
  static constexpr const char* aggs[] = {"count", "sum", "avg", "min", "max", "stddev", "variance", "median",
                                         "quantile", "p50", "p90", "p95", "p99", "p999", "distinct", "topk",
                                         "first", "last", "rate", "window_start", "window_end", "window_seconds"};
  std::string best;
  size_t best_d = 3;
  auto consider = [&](std::string_view cand) {
    const size_t d = edit_distance(name, cand);
    if (d < best_d) {
      best_d = d;
      best = std::string(cand);
    }
  };
  for (const char* a : aggs) consider(a);
  for (size_t i = 0; i < function_count(); ++i) consider(function_at(static_cast<uint32_t>(i)).name);
  return best;
}

class Compiler {
 public:
  Compiler(std::string_view src, Diagnostic& err) : src_(src), err_(err) {}

  std::vector<std::string>* fields = nullptr;  // destino de las rutas de campo
  QueryPlan* plan = nullptr;                   // destino de las agregaciones
  const std::vector<ExprPtr>* group = nullptr; // expresiones del GROUP BY
  std::vector<std::pair<std::string, uint32_t>> aliases;

  [[noreturn]] void fail(const SourcePos& pos, std::string msg) {
    err_.pos = pos;
    err_.message = std::move(msg);
    throw CompileFail{};
  }

  // Compila una expresión completa en un programa nuevo.
  void compile(const Expr& e, Program& out, const Ctx& c) {
    Emit em{&out};
    expr(e, em, c);
    em.op(OpCode::Ret);
    if (out.max_stack > kMaxStack) fail(e.pos, "expresión demasiado compleja (la pila de la VM no alcanza)");
  }

  void expr(const Expr& e, Emit& em, const Ctx& c) {
    if (c.scope == Scope::Row && group) {
      for (size_t i = 0; i < group->size(); ++i) {
        if (same_expr(*(*group)[i], e)) {
          em.op(OpCode::Key, static_cast<uint32_t>(i));
          em.push();
          return;
        }
      }
    }
    if (!is_literal(e) && is_constant(e)) {
      fold(e, em, c);
      return;
    }
    node(e, em, c);
  }

 private:
  static bool is_literal(const Expr& e) {
    return e.kind == ExprKind::Null || e.kind == ExprKind::Bool || e.kind == ExprKind::Number ||
           e.kind == ExprKind::String;
  }

  static bool is_constant(const Expr& e) {
    switch (e.kind) {
      case ExprKind::Null:
      case ExprKind::Bool:
      case ExprKind::Number:
      case ExprKind::String: return true;
      case ExprKind::Field:
      case ExprKind::Star: return false;
      case ExprKind::Call: {
        AggKind k;
        double q;
        if (aggregate_info(e.text, k, q) || is_window_function(e.text)) return false;
        const FunctionInfo* f = find_function(e.text);
        if (!f || !f->pure) return false;
        break;
      }
      default: break;
    }
    for (const auto& a : e.args)
      if (!is_constant(*a)) return false;
    return true;
  }

  // Plegado de constantes: evalúa la subexpresión ya en compilación.
  void fold(const Expr& e, Emit& em, const Ctx& c) {
    Program tmp;
    Emit t{&tmp};
    node(e, t, c);
    t.op(OpCode::Ret);
    Arena scratch(1024);
    EvalContext ctx;
    ctx.scratch = &scratch;
    const Value v = eval(tmp, ctx);
    em.op(OpCode::Const, em.p->add_const(v));
    em.push();
  }

  void constant(const Value& v, Emit& em) {
    em.op(OpCode::Const, em.p->add_const(v));
    em.push();
  }

  uint32_t intern_field(const std::string& path) {
    for (uint32_t i = 0; i < fields->size(); ++i)
      if ((*fields)[i] == path) return i;
    fields->push_back(path);
    return static_cast<uint32_t>(fields->size() - 1);
  }

  void node(const Expr& e, Emit& em, const Ctx& c) {
    switch (e.kind) {
      case ExprKind::Null: constant(Value::null(), em); return;
      case ExprKind::Bool: constant(Value::boolean(e.bool_value), em); return;
      case ExprKind::Number: constant(Value::number(e.number), em); return;
      case ExprKind::String: constant(Value::string(e.text), em); return;
      case ExprKind::Star: fail(e.pos, "'*' solo se admite dentro de count(*)");
      case ExprKind::Field: field(e, em, c); return;
      case ExprKind::Call: call(e, em, c); return;
      case ExprKind::Unary:
        expr(*e.args[0], em, c);
        em.op(e.op == Op::Neg ? OpCode::Neg : OpCode::Not);
        return;
      case ExprKind::Binary: binary(e, em, c); return;
      case ExprKind::In: in_list(e, em, c); return;
      case ExprKind::Between:
        expr(*e.args[0], em, c);
        expr(*e.args[1], em, c);
        em.op(OpCode::Ge);
        em.pop();
        expr(*e.args[0], em, c);
        expr(*e.args[2], em, c);
        em.op(OpCode::Le);
        em.pop();
        em.op(OpCode::And);
        em.pop();
        if (e.negated) em.op(OpCode::Not);
        return;
      case ExprKind::Like: {
        expr(*e.args[0], em, c);
        const Expr& pat = *e.args[1];
        if (pat.kind != ExprKind::String) fail(pat.pos, "el patrón de LIKE debe ser una cadena literal");
        em.p->likes.push_back(LikePattern{pat.text, e.case_insensitive});
        em.op(OpCode::Like, static_cast<uint32_t>(em.p->likes.size() - 1));
        if (e.negated) em.op(OpCode::Not);
        return;
      }
      case ExprKind::IsNull:
        expr(*e.args[0], em, c);
        em.op(e.negated ? OpCode::IsNotNull : OpCode::IsNull);
        return;
      case ExprKind::Case: case_expr(e, em, c); return;
    }
  }

  void field(const Expr& e, Emit& em, const Ctx& c) {
    if (c.scope == Scope::Row) {
      if (c.allow_aliases) {
        for (const auto& [name, idx] : aliases) {
          if (name == e.text) {
            em.op(OpCode::Col, idx);
            em.push();
            return;
          }
        }
      }
      fail(e.pos, cat("el campo '", e.text, "' no está en GROUP BY: úsalo dentro de una agregación (p. ej. max(",
                      e.text, ")) o añádelo a GROUP BY"));
    }
    em.op(OpCode::Field, intern_field(e.text));
    em.push();
  }

  void binary(const Expr& e, Emit& em, const Ctx& c) {
    if (e.op == Op::And || e.op == Op::Or) {
      expr(*e.args[0], em, c);
      const size_t jump = em.here();
      em.op(e.op == Op::And ? OpCode::JumpIfFalse : OpCode::JumpIfTrue);
      expr(*e.args[1], em, c);
      em.op(e.op == Op::And ? OpCode::And : OpCode::Or);
      em.pop();
      em.patch_to_here(jump);
      return;
    }
    expr(*e.args[0], em, c);
    expr(*e.args[1], em, c);
    OpCode op;
    switch (e.op) {
      case Op::Add: op = OpCode::Add; break;
      case Op::Sub: op = OpCode::Sub; break;
      case Op::Mul: op = OpCode::Mul; break;
      case Op::Div: op = OpCode::Div; break;
      case Op::Mod: op = OpCode::Mod; break;
      case Op::Concat: op = OpCode::Concat; break;
      case Op::Eq: op = OpCode::Eq; break;
      case Op::Ne: op = OpCode::Ne; break;
      case Op::Lt: op = OpCode::Lt; break;
      case Op::Le: op = OpCode::Le; break;
      case Op::Gt: op = OpCode::Gt; break;
      case Op::Ge: op = OpCode::Ge; break;
      default: fail(e.pos, "operador no soportado");
    }
    em.op(op);
    em.pop();
  }

  void in_list(const Expr& e, Emit& em, const Ctx& c) {
    bool all_const = true;
    for (size_t i = 1; i < e.args.size(); ++i) all_const = all_const && is_constant(*e.args[i]);
    if (all_const) {
      ValueSet set;
      Arena scratch(1024);
      for (size_t i = 1; i < e.args.size(); ++i) {
        Program tmp;
        Emit t{&tmp};
        node(*e.args[i], t, c);
        t.op(OpCode::Ret);
        EvalContext ctx;
        ctx.scratch = &scratch;
        const Value v = eval(tmp, ctx);
        if (v.type == VType::Bool) set.add(Value::number(v.num));
        else set.add(v);
      }
      set.seal();
      em.p->sets.push_back(std::move(set));
      expr(*e.args[0], em, c);
      em.op(OpCode::InSet, static_cast<uint32_t>(em.p->sets.size() - 1));
    } else {
      // Lista con expresiones: x = a OR x = b OR ...
      for (size_t i = 1; i < e.args.size(); ++i) {
        expr(*e.args[0], em, c);
        expr(*e.args[i], em, c);
        em.op(OpCode::Eq);
        em.pop();
        if (i > 1) {
          em.op(OpCode::Or);
          em.pop();
        }
      }
    }
    if (e.negated) em.op(OpCode::Not);
  }

  void case_expr(const Expr& e, Emit& em, const Ctx& c) {
    const size_t pairs = (e.args.size() - (e.has_else ? 1 : 0)) / 2;
    std::vector<size_t> to_end;
    const uint32_t base = em.depth;
    for (size_t i = 0; i < pairs; ++i) {
      expr(*e.args[2 * i], em, c);
      const size_t skip = em.here();
      em.op(OpCode::JumpIfNotTrue);
      em.pop();
      expr(*e.args[2 * i + 1], em, c);
      to_end.push_back(em.here());
      em.op(OpCode::Jump);
      em.depth = base;  // la otra rama empieza con la pila como antes
      em.patch_to_here(skip);
    }
    if (e.has_else)
      expr(*e.args.back(), em, c);
    else
      constant(Value::null(), em);
    for (size_t j : to_end) em.patch_to_here(j);
    em.depth = base + 1;
  }

  void call(const Expr& e, Emit& em, const Ctx& c) {
    AggKind kind;
    double q;
    if (aggregate_info(e.text, kind, q)) {
      if (c.scope == Scope::Event) {
        if (c.in_aggregate)
          fail(e.pos, cat("no se pueden anidar agregaciones: ", e.text, "() está dentro de otra agregación"));
        fail(e.pos, cat("la agregación ", e.text, "() no está permitida en ", c.clause,
                        "; para filtrar por un agregado usa HAVING"));
      }
      const uint32_t idx = aggregate(e, kind, q, c);
      em.op(OpCode::Agg, idx);
      em.push();
      return;
    }
    if (is_window_function(e.text)) {
      if (c.scope == Scope::Event)
        fail(e.pos, cat(e.text, "() solo tiene sentido en SELECT, HAVING, ORDER BY o ALERT"));
      if (!e.args.empty()) fail(e.pos, cat(e.text, "() no lleva argumentos"));
      em.op(e.text == "window_start" ? OpCode::WinStart : e.text == "window_end" ? OpCode::WinEnd : OpCode::WinSecs);
      em.push();
      return;
    }
    const FunctionInfo* f = find_function(e.text);
    if (!f) {
      const std::string s = suggest_function(e.text);
      fail(e.pos, s.empty() ? cat("función desconocida '", e.text, "'")
                            : cat("función desconocida '", e.text, "' (¿quisiste decir ", s, "?)"));
    }
    if (e.args.size() < f->min_args || e.args.size() > f->max_args) {
      fail(e.pos, f->min_args == f->max_args
                      ? cat(f->name, "() necesita ", static_cast<int>(f->min_args), " argumento(s): ", f->help)
                      : cat(f->name, "() admite entre ", static_cast<int>(f->min_args), " y ",
                            static_cast<int>(f->max_args), " argumentos: ", f->help));
    }
    for (const auto& a : e.args) {
      if (a->kind == ExprKind::Star) fail(a->pos, "'*' solo se admite dentro de count(*)");
      expr(*a, em, c);
    }
    em.op(OpCode::Call, function_id(f), static_cast<uint8_t>(e.args.size()));
    em.pop(static_cast<uint32_t>(e.args.size()));
    em.push();
  }

  double constant_number(const Expr& e, const char* what) {
    if (!is_constant(e)) fail(e.pos, cat(what, " debe ser un número constante"));
    Program tmp;
    Emit t{&tmp};
    node(e, t, Ctx{});
    t.op(OpCode::Ret);
    Arena scratch(256);
    EvalContext ctx;
    ctx.scratch = &scratch;
    const Value v = eval(tmp, ctx);
    if (v.type != VType::Num) fail(e.pos, cat(what, " debe ser un número constante"));
    return v.num;
  }

  uint32_t aggregate(const Expr& e, AggKind kind, double q, const Ctx& c) {
    AggSpec spec;
    spec.kind = kind;
    spec.q = q;
    const size_t argc = e.args.size();
    const bool star = argc == 1 && e.args[0]->kind == ExprKind::Star;
    auto need_args = [&](size_t lo, size_t hi, const char* usage) {
      if (argc < lo || argc > hi || star) fail(e.pos, cat("uso: ", usage));
    };
    switch (kind) {
      case AggKind::Count:
        if (argc == 0 || star) {
          spec.kind = AggKind::Count;
        } else if (argc == 1) {
          spec.kind = AggKind::CountValues;
          spec.has_arg = true;
        } else {
          fail(e.pos, "uso: count(*) o count(expresión)");
        }
        break;
      case AggKind::Quantile:
        if (e.text == "quantile" || e.text == "percentile") {
          need_args(2, 2, "quantile(x, 0.99) o percentile(x, 99)");
          double v = constant_number(*e.args[1], "el cuantil");
          if (e.text == "percentile" || v > 1) v /= 100.0;
          if (!(v >= 0 && v <= 1)) fail(e.args[1]->pos, "el cuantil debe estar entre 0 y 1 (o 0 y 100 en percentile)");
          spec.q = v;
        } else {
          need_args(1, 1, cat(e.text, "(x)").c_str());
        }
        spec.has_arg = true;
        break;
      case AggKind::TopK: {
        need_args(1, 2, "topk(x) o topk(x, k)");
        double k = 10;
        if (argc == 2) k = constant_number(*e.args[1], "k en topk(x, k)");
        if (k != std::floor(k) || k < 1 || k > 100) fail(e.pos, "k en topk(x, k) debe ser un entero entre 1 y 100");
        spec.k = static_cast<uint32_t>(k);
        spec.has_arg = true;
        break;
      }
      case AggKind::Rate:
        if (argc == 0 || star) {
          spec.has_arg = false;
        } else {
          need_args(1, 1, "rate() o rate(x)");
          spec.has_arg = true;
        }
        break;
      default:
        need_args(1, 1, cat(e.text, "(x)").c_str());
        spec.has_arg = true;
        break;
    }
    // Firma canónica: tipo + parámetros + argumento.
    spec.signature = agg_kind_name(spec.kind);
    if (spec.kind == AggKind::Quantile) {
      spec.signature += '@';
      append_double(spec.signature, spec.q, false);
    }
    if (spec.kind == AggKind::TopK) {
      spec.signature += '@';
      append_uint(spec.signature, spec.k);
    }
    spec.signature += '(';
    if (spec.has_arg) spec.signature += canonical(*e.args[0]);
    spec.signature += ')';
    for (uint32_t i = 0; i < plan->aggs.size(); ++i)
      if (plan->aggs[i].signature == spec.signature) return i;
    if (spec.has_arg) {
      Ctx ac;
      ac.scope = Scope::Event;
      ac.clause = c.clause;
      ac.in_aggregate = true;
      const std::vector<ExprPtr>* saved = group;
      group = nullptr;
      compile(*e.args[0], spec.arg, ac);
      group = saved;
    }
    plan->aggs.push_back(std::move(spec));
    return static_cast<uint32_t>(plan->aggs.size() - 1);
  }

  std::string_view src_;
  Diagnostic& err_;
};

bool uses_aggregate(const Expr& e) {
  if (e.kind == ExprKind::Call) {
    AggKind k;
    double q;
    if (aggregate_info(e.text, k, q)) return true;
  }
  for (const auto& a : e.args)
    if (uses_aggregate(*a)) return true;
  return false;
}

std::string column_name(const SelectItem& item) {
  if (!item.alias.empty()) return item.alias;
  if (item.expr->kind == ExprKind::Field) return item.expr->text;
  if (item.expr->kind == ExprKind::Call) return canonical(*item.expr);
  return item.source;
}

}  // namespace

bool compile_query(std::string_view text, const CompileOptions& opts, QueryPlan& out, Diagnostic& err) {
  QueryAst ast;
  if (!parse_query(text, ast, err)) return false;
  out = QueryPlan();
  out.text = std::string(text);
  out.stream = ast.from;
  Compiler cc(text, err);
  cc.fields = &out.fields;
  cc.plan = &out;
  try {
    // Ventana.
    if (ast.window.present) {
      out.window_type = ast.window.type;
      out.window_ms = ast.window.size_ms;
      out.advance_ms = ast.window.advance_ms;
    } else {
      out.window_type = WindowType::Tumbling;
      out.window_ms = out.advance_ms = opts.default_window_ms;
    }
    const SourcePos wpos = ast.window.present ? ast.window.pos : ast.from_pos;
    if (out.window_ms < opts.min_window_ms || out.window_ms > opts.max_window_ms)
      cc.fail(wpos, cat("la ventana debe durar entre ", format_duration(opts.min_window_ms), " y ",
                        format_duration(opts.max_window_ms)));
    if (out.advance_ms <= 0 || out.advance_ms > out.window_ms)
      cc.fail(wpos, "el salto (EVERY) debe ser positivo y no mayor que la ventana");
    if (out.window_ms % out.advance_ms != 0)
      cc.fail(wpos, cat("la ventana (", format_duration(out.window_ms), ") debe ser múltiplo del salto (",
                        format_duration(out.advance_ms), ")"));
    if (out.panes_per_window() > opts.max_panes_per_window)
      cc.fail(wpos, cat("demasiados paneles por ventana (", out.panes_per_window(), "); el máximo es ",
                        opts.max_panes_per_window, ": usa un salto mayor"));
    out.lateness_ms = ast.window.lateness_ms >= 0 ? ast.window.lateness_ms : opts.default_lateness_ms;
    if (out.lateness_ms > opts.max_lateness_ms)
      cc.fail(wpos, cat("LATENESS no puede superar ", format_duration(opts.max_lateness_ms)));
    out.emit_every_ms = ast.emit_every_ms;
    if (out.emit_every_ms != 0) {
      if (out.emit_every_ms < opts.min_emit_ms)
        cc.fail(ast.emit_pos, cat("EMIT EVERY debe ser de al menos ", format_duration(opts.min_emit_ms)));
      if (out.emit_every_ms >= out.window_ms)
        cc.fail(ast.emit_pos, "EMIT EVERY debe ser menor que la ventana (si no, basta con el resultado final)");
    }

    // WHERE (ámbito de evento).
    if (ast.where) {
      Ctx c;
      c.scope = Scope::Event;
      c.clause = "WHERE";
      cc.compile(*ast.where, out.where, c);
      out.has_where = true;
      out.where_text = canonical(*ast.where);
    }

    // GROUP BY (ámbito de evento).
    for (const auto& g : ast.group_by) {
      Ctx c;
      c.scope = Scope::Event;
      c.clause = "GROUP BY";
      if (g->kind == ExprKind::Number) cc.fail(g->pos, "GROUP BY por posición no está soportado: escribe la expresión");
      Program p;
      cc.compile(*g, p, c);
      out.keys.push_back(std::move(p));
      out.key_names.push_back(g->kind == ExprKind::Field ? g->text : canonical(*g));
    }
    cc.group = &ast.group_by;

    // SELECT (ámbito de fila).
    bool any_agg = false;
    for (const auto& item : ast.select) any_agg = any_agg || uses_aggregate(*item.expr);
    if (!any_agg && ast.group_by.empty())
      cc.fail(ast.select.front().pos,
              "una consulta continua resume eventos por ventana: usa una agregación (count, avg, p99, distinct...) o "
              "GROUP BY; para ver eventos sueltos usa el tail en vivo");
    for (const auto& item : ast.select) {
      Ctx c;
      c.scope = Scope::Row;
      c.clause = "SELECT";
      OutputColumn col;
      col.name = column_name(item);
      std::string base = col.name;
      for (int n = 2; std::any_of(out.columns.begin(), out.columns.end(), [&](const OutputColumn& o) {
             return o.name == col.name;
           });
           ++n)
        col.name = cat(base, "_", n);
      cc.compile(*item.expr, col.prog, c);
      out.columns.push_back(std::move(col));
    }
    for (uint32_t i = 0; i < out.columns.size(); ++i) cc.aliases.emplace_back(out.columns[i].name, i);

    // HAVING, ALERT y ORDER BY: ámbito de fila con alias visibles.
    if (ast.having) {
      Ctx c;
      c.scope = Scope::Row;
      c.clause = "HAVING";
      c.allow_aliases = true;
      cc.compile(*ast.having, out.having, c);
      out.has_having = true;
      out.having_text = canonical(*ast.having);
    }
    if (ast.alert) {
      Ctx c;
      c.scope = Scope::Row;
      c.clause = "ALERT IF";
      c.allow_aliases = true;
      cc.compile(*ast.alert, out.alert, c);
      out.has_alert = true;
      out.alert_text = ast.alert_source;
      out.alert_for = ast.alert_for;
    }
    for (const auto& o : ast.order_by) {
      Ctx c;
      c.scope = Scope::Row;
      c.clause = "ORDER BY";
      c.allow_aliases = true;
      OrderKey key;
      key.desc = o.desc;
      if (o.expr->kind == ExprKind::Number) {
        // ORDER BY 2: posición de columna (desde 1).
        const double n = o.expr->number;
        if (n != std::floor(n) || n < 1 || n > static_cast<double>(out.columns.size()))
          cc.fail(o.expr->pos, cat("ORDER BY ", n, ": no hay tal columna (hay ", out.columns.size(), ")"));
        Emit em{&key.prog};
        em.op(OpCode::Col, static_cast<uint32_t>(n - 1));
        em.push();
        em.op(OpCode::Ret);
        key.text = out.columns[static_cast<size_t>(n - 1)].name;
      } else {
        cc.compile(*o.expr, key.prog, c);
        key.text = o.expr->kind == ExprKind::Field ? o.expr->text : canonical(*o.expr);
      }
      out.order.push_back(std::move(key));
    }
    out.limit = ast.limit;
  } catch (const CompileFail&) {
    return false;
  }
  return true;
}

bool compile_filter(std::string_view text, FilterPlan& out, Diagnostic& err) {
  ExprPtr e;
  if (!parse_expression(text, e, err)) return false;
  out = FilterPlan();
  out.text = std::string(text);
  Compiler cc(text, err);
  cc.fields = &out.fields;
  QueryPlan dummy;
  cc.plan = &dummy;
  try {
    Ctx c;
    c.scope = Scope::Event;
    c.clause = "el filtro";
    cc.compile(*e, out.prog, c);
  } catch (const CompileFail&) {
    return false;
  }
  return true;
}

// ----------------------------------------------------------------------------
// Explicación del plan
// ----------------------------------------------------------------------------

std::string QueryPlan::explain(bool with_bytecode) const {
  std::string s;
  s += cat("Consulta continua sobre el stream '", stream, "'\n");
  if (has_where) s += cat("  Filtro (WHERE):   ", where_text, "\n");
  if (!keys.empty()) {
    s += "  Agrupa por:       ";
    for (size_t i = 0; i < key_names.size(); ++i) s += cat(i ? ", " : "", key_names[i]);
    s += "\n";
  } else {
    s += "  Agrupa por:       (global, una fila por ventana)\n";
  }
  if (window_type == WindowType::Tumbling)
    s += cat("  Ventana:          tumbling de ", format_duration(window_ms));
  else
    s += cat("  Ventana:          hopping de ", format_duration(window_ms), " cada ", format_duration(advance_ms),
             " (", panes_per_window(), " paneles de ", format_duration(advance_ms), " que se fusionan)");
  s += cat(", lateness ", format_duration(lateness_ms), "\n");
  s += "  Agregaciones:\n";
  for (size_t i = 0; i < aggs.size(); ++i) {
    const AggSpec& a = aggs[i];
    std::string how;
    switch (a.kind) {
      case AggKind::Count:
      case AggKind::CountValues: how = "contador exacto"; break;
      case AggKind::Sum:
      case AggKind::Rate: how = "suma exacta"; break;
      case AggKind::Avg:
      case AggKind::Stddev:
      case AggKind::Variance: how = "momentos de Welford (exactos, fusionables)"; break;
      case AggKind::Min:
      case AggKind::Max: how = "exacto"; break;
      case AggKind::Quantile: how = cat("DDSketch q=", a.q, ", error relativo <= 1 %"); break;
      case AggKind::Distinct: how = "HyperLogLog p=14 (disperso -> denso), error típico ~0,8 %"; break;
      case AggKind::TopK: how = cat("Space-Saving, k=", a.k, " con ", std::max<uint32_t>(64, a.k * 8), " contadores"); break;
      case AggKind::First:
      case AggKind::Last: how = "por tiempo de evento"; break;
    }
    s += cat("    #", i, "  ", a.signature, "  -> ", how, "\n");
  }
  s += "  Columnas:         ";
  for (size_t i = 0; i < columns.size(); ++i) s += cat(i ? ", " : "", columns[i].name);
  s += "\n";
  if (has_having) s += cat("  HAVING:           ", having_text, "\n");
  if (!order.empty()) {
    s += "  Orden:            ";
    for (size_t i = 0; i < order.size(); ++i) s += cat(i ? ", " : "", order[i].text, order[i].desc ? " DESC" : " ASC");
    s += "\n";
  }
  if (limit > 0) s += cat("  Límite:           ", limit, " filas por ventana\n");
  if (emit_every_ms > 0) s += cat("  Parciales:        cada ", format_duration(emit_every_ms), "\n");
  if (has_alert)
    s += cat("  Alerta:           ", alert_text, alert_for > 1 ? cat(" durante ", alert_for, " ventanas") : std::string(), "\n");
  s += "  Campos leídos:    ";
  for (size_t i = 0; i < fields.size(); ++i) s += cat(i ? ", " : "", fields[i]);
  if (fields.empty()) s += "(ninguno)";
  s += "\n";
  if (with_bytecode) {
    if (has_where) s += cat("\n  [WHERE]\n", where.disassemble());
    for (size_t i = 0; i < keys.size(); ++i) s += cat("\n  [GROUP BY ", key_names[i], "]\n", keys[i].disassemble());
    for (size_t i = 0; i < aggs.size(); ++i)
      if (aggs[i].has_arg) s += cat("\n  [AGG #", i, " argumento]\n", aggs[i].arg.disassemble());
    for (const auto& c : columns) s += cat("\n  [SELECT ", c.name, "]\n", c.prog.disassemble());
    if (has_having) s += cat("\n  [HAVING]\n", having.disassemble());
    if (has_alert) s += cat("\n  [ALERT IF]\n", alert.disassemble());
  }
  return s;
}

}  // namespace zr::zql
