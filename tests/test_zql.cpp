#include <algorithm>
#include <string>
#include <vector>

#include "core/arena.hpp"
#include "framework.hpp"
#include "zql/lexer.hpp"
#include "zql/parser.hpp"
#include "zql/plan.hpp"
#include "zql/vm.hpp"

using namespace zr;
using namespace zr::zql;

namespace {

// Evalúa un filtro contra un evento dado como pares (campo, valor).
struct Eval {
  FilterPlan plan;
  Diagnostic err;
  bool ok = false;
  explicit Eval(std::string_view text) { ok = compile_filter(text, plan, err); }

  Value run(const std::vector<std::pair<std::string, Value>>& event, Arena& scratch) const {
    std::vector<Value> slots(plan.fields.size());
    for (size_t i = 0; i < plan.fields.size(); ++i)
      for (const auto& [k, v] : event)
        if (k == plan.fields[i]) slots[i] = v;
    EvalContext ctx;
    ctx.fields = slots.data();
    ctx.scratch = &scratch;
    return eval(plan.prog, ctx);
  }
};

std::string show(const Value& v) {
  std::string s;
  write_json(s, v);
  return s;
}

std::string run_expr(std::string_view text, const std::vector<std::pair<std::string, Value>>& event = {}) {
  Eval e(text);
  if (!e.ok) return "ERROR: " + e.err.to_string();
  Arena scratch;
  return show(e.run(event, scratch));
}

}  // namespace

TEST(lexer_reconoce_duraciones_cadenas_y_comentarios) {
  std::vector<Token> toks;
  Diagnostic err;
  REQUIRE(tokenize("SELECT p99(x) -- comentario\n FROM \"mi-stream\" /* otro */ WINDOW TUMBLING 1h30m "
                   "WHERE s = 'it''s' AND n >= 1.5e2",
                   toks, err));
  std::vector<Tok> kinds;
  for (const auto& t : toks) kinds.push_back(t.kind);
  const std::vector<Tok> expected = {Tok::Ident, Tok::Ident,    Tok::LParen, Tok::Ident, Tok::RParen, Tok::Ident,
                                     Tok::Ident, Tok::Ident,    Tok::Ident,  Tok::Duration, Tok::Ident, Tok::Ident,
                                     Tok::Eq,    Tok::String,   Tok::Ident,  Tok::Ident, Tok::Ge,     Tok::Number,
                                     Tok::End};
  CHECK(kinds == expected);
  CHECK(toks[6].quoted);
  CHECK_EQ(toks[6].text, std::string("mi-stream"));
  CHECK_EQ(toks[9].duration_ms, int64_t{5400000});
  CHECK_EQ(toks[13].text, std::string("it's"));
  CHECK_EQ(toks[17].number, 150.0);
  CHECK_EQ(toks[5].pos.line, 2);
}

TEST(lexer_rutas_con_indices_de_array) {
  std::vector<Token> toks;
  Diagnostic err;
  // "tags.0.name" y "m.1.2" son rutas; ".5" suelto y "1.5" siguen siendo números.
  REQUIRE(tokenize("tags.0.name = m.1.2 AND x > .5 AND y < 1.5 AND `a-b`.3", toks, err));
  std::vector<Tok> kinds;
  for (const auto& t : toks) kinds.push_back(t.kind);
  const std::vector<Tok> expected = {Tok::Ident,  Tok::Dot,    Tok::Number, Tok::Dot,    Tok::Ident, Tok::Eq,
                                     Tok::Ident,  Tok::Dot,    Tok::Number, Tok::Dot,    Tok::Number, Tok::Ident,
                                     Tok::Ident,  Tok::Gt,     Tok::Number, Tok::Ident,  Tok::Ident,  Tok::Lt,
                                     Tok::Number, Tok::Ident,  Tok::Ident,  Tok::Dot,    Tok::Number, Tok::End};
  CHECK(kinds == expected);
  CHECK_EQ(toks[2].text, std::string("0"));
  CHECK_EQ(toks[10].text, std::string("2"));
  CHECK_EQ(toks[14].number, 0.5);
  CHECK_EQ(toks[18].number, 1.5);
  // Y el compilador las convierte en rutas que lee el escáner.
  zql::QueryPlan plan;
  REQUIRE(zql::compile_query("SELECT tags.0 AS t, count(*) AS n FROM s WHERE http.codes.1 >= 500 GROUP BY tags.0",
                             zql::CompileOptions{}, plan, err));
  CHECK(std::find(plan.fields.begin(), plan.fields.end(), "tags.0") != plan.fields.end());
  CHECK(std::find(plan.fields.begin(), plan.fields.end(), "http.codes.1") != plan.fields.end());
}

TEST(lexer_informa_linea_y_columna) {
  std::vector<Token> toks;
  Diagnostic err;
  CHECK(!tokenize("SELECT 1\nFROM x WHERE a = 'sin cerrar", toks, err));
  CHECK_EQ(err.pos.line, 2);
  CHECK_EQ(err.pos.col, 18);
  CHECK(!tokenize("SELECT 10q FROM x", toks, err));
  CHECK(err.message.find("duración") != std::string::npos);
}

TEST(parser_consulta_completa_en_cualquier_orden) {
  QueryAst q;
  Diagnostic err;
  REQUIRE(parse_query("SELECT service, count(*) AS n, p99(latency_ms) p99 FROM requests "
                      "WINDOW HOPPING 1m EVERY 10s LATENESS 5s WHERE status >= 500 GROUP BY service "
                      "HAVING n > 10 ORDER BY p99 DESC LIMIT 5 EMIT EVERY 1s ALERT IF p99 > 800 FOR 3 WINDOWS;",
                      q, err));
  CHECK_EQ(q.select.size(), size_t{3});
  CHECK_EQ(q.select[1].alias, std::string("n"));
  CHECK_EQ(q.select[2].alias, std::string("p99"));
  CHECK_EQ(q.from, std::string("requests"));
  CHECK(q.window.type == WindowType::Hopping);
  CHECK_EQ(q.window.size_ms, int64_t{60000});
  CHECK_EQ(q.window.advance_ms, int64_t{10000});
  CHECK_EQ(q.window.lateness_ms, int64_t{5000});
  CHECK(q.where && q.having && q.alert);
  CHECK_EQ(q.alert_for, uint32_t{3});
  CHECK_EQ(q.emit_every_ms, int64_t{1000});
  CHECK_EQ(q.limit, int64_t{5});
  REQUIRE(q.order_by.size() == 1);
  CHECK(q.order_by[0].desc);
  CHECK_EQ(q.alert_source, std::string("p99 > 800"));
}

TEST(parser_errores_claros) {
  struct Case {
    const char* text;
    const char* fragment;
  };
  const Case cases[] = {
      {"SELECT count(*) requests", "FROM"},
      {"SELECT count(*) FROM r WHERE a = 1 WHERE b = 2", "dos veces"},
      {"SELECT count(*) FROM r WINDOW ROLLING 10s", "TUMBLING o HOPPING"},
      {"SELECT count(*) FROM r WINDOW HOPPING 1m", "EVERY"},
      {"SELECT count(*) FROM r GROUP service", "BY"},
      {"SELECT count(* FROM r", "')'"},
      {"SELECT a FROM r WHERE x IN 1", "'('"},
      {"SELECT count(*) FROM r LIMIT 0", "entre 1 y"},
      {"SELECT count(*) AS from FROM r", "reservada"},
      {"SELECT count(*) FROM r ALERT n > 1", "IF"},
  };
  for (const auto& c : cases) {
    QueryAst q;
    Diagnostic err;
    if (parse_query(c.text, q, err)) {
      zt::report_failure(__FILE__, __LINE__, cat("aceptó: ", c.text));
    } else if (err.message.find(c.fragment) == std::string::npos) {
      zt::report_failure(__FILE__, __LINE__, cat("mensaje inesperado para '", c.text, "': ", err.message));
    }
  }
}

TEST(vm_aritmetica_y_semantica_de_nulos) {
  CHECK_EQ(run_expr("1 + 2 * 3"), std::string("7"));
  CHECK_EQ(run_expr("(1 + 2) * 3 - 4 / 2"), std::string("7"));
  CHECK_EQ(run_expr("7 % 3"), std::string("1"));
  CHECK_EQ(run_expr("1 / 0"), std::string("null"));
  CHECK_EQ(run_expr("-x", {{"x", Value::number(5)}}), std::string("-5"));
  CHECK_EQ(run_expr("x + 1"), std::string("null"));  // campo ausente
  CHECK_EQ(run_expr("x > 3", {{"x", Value::number(5)}}), std::string("true"));
  CHECK_EQ(run_expr("x > 3"), std::string("null"));
  // Lógica trivalente de SQL.
  CHECK_EQ(run_expr("x > 3 AND false"), std::string("false"));
  CHECK_EQ(run_expr("x > 3 OR true"), std::string("true"));
  CHECK_EQ(run_expr("x > 3 AND true"), std::string("null"));
  CHECK_EQ(run_expr("NOT (x > 3)"), std::string("null"));
  CHECK_EQ(run_expr("x IS NULL"), std::string("true"));
  CHECK_EQ(run_expr("x IS NOT NULL", {{"x", Value::number(0)}}), std::string("true"));
  // Booleanos como 0/1 en aritmética.
  CHECK_EQ(run_expr("(s >= 500) * 100", {{"s", Value::number(503)}}), std::string("100"));
  CHECK_EQ(run_expr("'a' = 1"), std::string("false"));
  CHECK_EQ(run_expr("'abc' < 'abd'"), std::string("true"));
  CHECK_EQ(run_expr("2s + 500ms"), std::string("2500"));
}

TEST(vm_in_like_between_case) {
  const std::vector<std::pair<std::string, Value>> ev = {
      {"route", Value::string("/api/cart/42")}, {"status", Value::number(404)}, {"region", Value::string("sa-east-1")}};
  CHECK_EQ(run_expr("status IN (200, 404, 500)", ev), std::string("true"));
  CHECK_EQ(run_expr("status NOT IN (200, 500)", ev), std::string("true"));
  CHECK_EQ(run_expr("region IN ('us-east-1', 'eu-west-1')", ev), std::string("false"));
  CHECK_EQ(run_expr("status IN (status, 1)", ev), std::string("true"));  // lista no constante
  CHECK_EQ(run_expr("route LIKE '/api/%'", ev), std::string("true"));
  CHECK_EQ(run_expr("route LIKE '/api/c_rt/%'", ev), std::string("true"));
  CHECK_EQ(run_expr("route NOT LIKE '%/42'", ev), std::string("false"));
  CHECK_EQ(run_expr("route ILIKE '/API/%'", ev), std::string("true"));
  CHECK_EQ(run_expr("route LIKE '/api'", ev), std::string("false"));
  CHECK_EQ(run_expr("status BETWEEN 400 AND 499", ev), std::string("true"));
  CHECK_EQ(run_expr("status NOT BETWEEN 400 AND 499", ev), std::string("false"));
  CHECK_EQ(run_expr("CASE WHEN status >= 500 THEN 'error' WHEN status >= 400 THEN 'cliente' ELSE 'ok' END", ev),
           std::string("\"cliente\""));
  CHECK_EQ(run_expr("CASE WHEN status > 999 THEN 1 END", ev), std::string("null"));
  // LIKE con UTF-8: '_' es un carácter, no un byte.
  CHECK_EQ(run_expr("n LIKE 'a_o'", {{"n", Value::string("a\xc3\xb1o")}}), std::string("true"));
}

TEST(vm_funciones_escalares) {
  const std::vector<std::pair<std::string, Value>> ev = {{"route", Value::string("/api/cart/42")},
                                                          {"name", Value::string("  Lima  ")},
                                                          {"lat", Value::number(123.456)}};
  CHECK_EQ(run_expr("upper(split_part(route, '/', 3))", ev), std::string("\"CART\""));
  CHECK_EQ(run_expr("trim(name) || '!'", ev), std::string("\"Lima!\""));
  CHECK_EQ(run_expr("round(lat, 1)", ev), std::string("123.5"));
  CHECK_EQ(run_expr("bucket(lat, 50)", ev), std::string("100"));
  CHECK_EQ(run_expr("coalesce(missing, 'def')", ev), std::string("\"def\""));
  CHECK_EQ(run_expr("if(lat > 100, 'lento', 'rápido')", ev), std::string("\"lento\""));
  CHECK_EQ(run_expr("substr('añoñito', 2, 3)"), std::string("\"ñoñ\""));
  CHECK_EQ(run_expr("length('año')"), std::string("3"));
  CHECK_EQ(run_expr("num('12.5') + 1"), std::string("13.5"));
  CHECK_EQ(run_expr("concat('a', null, 1, true)"), std::string("\"a1true\""));
  CHECK_EQ(run_expr("replace(route, '/', '.')", ev), std::string("\".api.cart.42\""));
  CHECK_EQ(run_expr("greatest(1, 7, 3) - least(4, 2)"), std::string("5"));
  CHECK(run_expr("lowr('x')").find("¿quisiste decir lower?") != std::string::npos);
  CHECK(run_expr("round()").find("argumentos") != std::string::npos);
}

TEST(compilador_pliega_constantes) {
  FilterPlan p;
  Diagnostic err;
  REQUIRE(compile_filter("latency_ms > 60 * 1000 / 2 AND region = lower('LIMA')", p, err));
  const std::string dis = p.prog.disassemble();
  CHECK(dis.find("CONST 30000") != std::string::npos);
  CHECK(dis.find("CONST 'lima'") != std::string::npos);
  CHECK(dis.find("MUL") == std::string::npos);
  CHECK(dis.find("CALL") == std::string::npos);
  CHECK_EQ(p.fields.size(), size_t{2});
}

TEST(compilador_plan_de_consulta) {
  QueryPlan plan;
  Diagnostic err;
  REQUIRE(compile_query("SELECT service, count(*) AS n, p99(latency_ms) AS p99, avg(status >= 500) * 100 AS err, "
                        "topk(route, 3) AS rutas FROM requests WHERE region != 'test' GROUP BY service "
                        "WINDOW TUMBLING 30s HAVING n > 1 AND p99(latency_ms) > 0 ORDER BY 3 DESC LIMIT 10 "
                        "ALERT IF err > 5 FOR 2",
                        CompileOptions{}, plan, err));
  CHECK_EQ(plan.stream, std::string("requests"));
  CHECK_EQ(plan.columns.size(), size_t{5});
  CHECK_EQ(plan.columns[2].name, std::string("p99"));
  // p99(latency_ms) aparece en SELECT y en HAVING pero se calcula una vez.
  CHECK_EQ(plan.aggs.size(), size_t{4});
  CHECK(plan.aggs[1].kind == AggKind::Quantile);
  CHECK_NEAR(plan.aggs[1].q, 0.99, 1e-12);
  CHECK(plan.aggs[3].kind == AggKind::TopK);
  CHECK_EQ(plan.aggs[3].k, uint32_t{3});
  CHECK_EQ(plan.window_ms, int64_t{30000});
  CHECK(plan.has_alert);
  CHECK_EQ(plan.alert_for, uint32_t{2});
  CHECK_EQ(plan.limit, int64_t{10});
  REQUIRE(plan.order.size() == 1);
  CHECK_EQ(plan.order[0].text, std::string("p99"));
  // Campos: region (WHERE), service (clave), latency_ms, status, route.
  CHECK_EQ(plan.fields.size(), size_t{5});
  const std::string ex = plan.explain(true);
  CHECK(ex.find("DDSketch") != std::string::npos);
  CHECK(ex.find("Space-Saving") != std::string::npos);
  CHECK(ex.find("[HAVING]") != std::string::npos);
}

TEST(compilador_rechaza_consultas_invalidas) {
  struct Case {
    const char* text;
    const char* fragment;
  };
  const Case cases[] = {
      {"SELECT service, count(*) FROM r", "GROUP BY"},
      {"SELECT latency_ms FROM r", "agregación"},
      {"SELECT count(*) FROM r WHERE count(*) > 1", "HAVING"},
      {"SELECT sum(count(*)) FROM r", "anidar"},
      {"SELECT count(*) FROM r WINDOW HOPPING 1m EVERY 7s", "múltiplo"},
      {"SELECT count(*) FROM r WINDOW TUMBLING 10ms", "entre"},
      {"SELECT count(*) FROM r WINDOW TUMBLING 10s EMIT EVERY 10s", "menor que la ventana"},
      {"SELECT count(*) FROM r ORDER BY 3", "no hay tal columna"},
      {"SELECT topk(route, 500) FROM r", "entre 1 y 100"},
      {"SELECT quantile(x, y) FROM r", "constante"},
      {"SELECT p99(*) FROM r", "uso:"},
      {"SELECT count(*) FROM r HAVING nope > 1", "no está en GROUP BY"},
      {"SELECT window_start(1), count(*) FROM r", "no lleva argumentos"},
      {"SELECT count(*) FROM r WHERE window_end() > 0", "solo tiene sentido"},
  };
  for (const auto& c : cases) {
    QueryPlan plan;
    Diagnostic err;
    if (compile_query(c.text, CompileOptions{}, plan, err)) {
      zt::report_failure(__FILE__, __LINE__, cat("compiló: ", c.text));
    } else if (err.message.find(c.fragment) == std::string::npos) {
      zt::report_failure(__FILE__, __LINE__, cat("mensaje inesperado para '", c.text, "': ", err.message));
    }
  }
}

TEST(compilador_resuelve_claves_y_alias_en_fila) {
  QueryPlan plan;
  Diagnostic err;
  REQUIRE(compile_query("SELECT bucket(latency_ms, 100) AS b, count(*) AS n, n * 2 AS doble FROM r "
                        "GROUP BY bucket(latency_ms, 100)",
                        CompileOptions{}, plan, err) == false);  // 'n' no es visible dentro de SELECT
  REQUIRE(compile_query("SELECT bucket(latency_ms, 100) AS b, count(*) AS n FROM r GROUP BY bucket(latency_ms, 100) "
                        "HAVING b >= 100 AND n > 0",
                        CompileOptions{}, plan, err));
  // La primera columna es la clave 0 directamente.
  CHECK(plan.columns[0].prog.disassemble().find("KEY #0") != std::string::npos);
  CHECK(plan.having.disassemble().find("COL #0") != std::string::npos);
  // Valores por defecto de la ventana.
  CHECK_EQ(plan.window_ms, int64_t{10000});
  CHECK_EQ(plan.lateness_ms, int64_t{2000});
}
