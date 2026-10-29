#include "zql/parser.hpp"

#include <cmath>
#include <initializer_list>

#include "core/strings.hpp"
#include "zql/lexer.hpp"

namespace zr::zql {

namespace {

constexpr const char* kReserved[] = {
    "select", "from", "where", "group", "by",  "having", "order", "limit", "window", "emit",
    "alert",  "and",  "or",    "not",   "in",  "is",     "null",  "true",  "false",  "like",
    "ilike",  "between", "case", "when", "then", "else", "end", "as",
};

bool is_reserved(std::string_view word) noexcept {
  for (const char* r : kReserved)
    if (iequals(word, r)) return true;
  return false;
}

struct ParseFail {};

class Parser {
 public:
  Parser(std::string_view src, std::vector<Token>&& toks, Diagnostic& err)
      : src_(src), toks_(std::move(toks)), err_(err) {}

  bool query(QueryAst& q) {
    try {
      parse_query(q);
      return true;
    } catch (const ParseFail&) {
      return false;
    }
  }

  bool expression_only(ExprPtr& out) {
    try {
      out = expr();
      if (peek().kind == Tok::Semicolon) next();
      if (peek().kind != Tok::End) fail_at(peek(), cat("sobra texto a partir de ", describe(peek())));
      return true;
    } catch (const ParseFail&) {
      return false;
    }
  }

 private:
  // ---------------------------------------------------------------- utilidades
  const Token& peek(size_t k = 0) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
  const Token& next() {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    last_end_ = t.end_offset;
    return t;
  }

  [[noreturn]] void fail_at(const Token& t, std::string msg) {
    err_.pos = t.pos;
    err_.message = std::move(msg);
    throw ParseFail{};
  }

  [[noreturn]] void fail_at(SourcePos p, std::string msg) {
    err_.pos = p;
    err_.message = std::move(msg);
    throw ParseFail{};
  }

  static std::string describe(const Token& t) {
    switch (t.kind) {
      case Tok::Ident: return cat(t.quoted ? "el identificador \"" : "'", t.text, t.quoted ? "\"" : "'");
      case Tok::String: return cat("la cadena '", t.text, "'");
      case Tok::Number:
      case Tok::Duration: return cat("'", t.text, "'");
      default: return token_name(t.kind);
    }
  }

  bool is_kw(const Token& t, const char* kw) const { return t.kind == Tok::Ident && !t.quoted && iequals(t.text, kw); }
  bool at_kw(const char* kw) const { return is_kw(peek(), kw); }
  bool accept_kw(const char* kw) {
    if (!at_kw(kw)) return false;
    next();
    return true;
  }
  void expect_kw(const char* kw, const char* context = nullptr) {
    if (!accept_kw(kw)) {
      std::string shown(kw);
      for (char& ch : shown)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
      std::string msg = cat("se esperaba ", shown);
      if (context) msg += cat(" ", context);
      fail_at(peek(), cat(msg, " y se encontró ", describe(peek())));
    }
  }
  bool accept(Tok k) {
    if (peek().kind != k) return false;
    next();
    return true;
  }
  void expect(Tok k, const char* context = nullptr) {
    if (!accept(k)) {
      std::string msg = cat("se esperaba ", token_name(k));
      if (context) msg += cat(" ", context);
      fail_at(peek(), cat(msg, " y se encontró ", describe(peek())));
    }
  }

  int64_t expect_duration(const char* context) {
    const Token& t = peek();
    if (t.kind != Tok::Duration) fail_at(t, cat("se esperaba una duración (p. ej. 10s, 1m, 500ms) ", context));
    next();
    return t.duration_ms;
  }

  int64_t expect_integer(const char* context, int64_t lo, int64_t hi) {
    const Token& t = peek();
    if (t.kind != Tok::Number || t.number != std::floor(t.number))
      fail_at(t, cat("se esperaba un número entero ", context));
    if (t.number < static_cast<double>(lo) || t.number > static_cast<double>(hi))
      fail_at(t, cat("el valor ", context, " debe estar entre ", lo, " y ", hi));
    next();
    return static_cast<int64_t>(t.number);
  }

  ExprPtr make(ExprKind k, const Token& at) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->pos = at.pos;
    return e;
  }

  // ------------------------------------------------------------------ consulta
  void parse_query(QueryAst& q) {
    expect_kw("select", "al principio de la consulta");
    do {
      SelectItem item;
      item.pos = peek().pos;
      const size_t start = peek().pos.offset;
      item.expr = expr();
      item.source = std::string(trim(src_.substr(start, last_end_ - start)));
      if (accept_kw("as")) {
        const Token& a = peek();
        if (a.kind != Tok::Ident) fail_at(a, "se esperaba un nombre de columna tras AS");
        if (!a.quoted && is_reserved(a.text))
          fail_at(a, cat("'", a.text, "' es una palabra reservada; ponla entre comillas dobles"));
        item.alias = a.text;
        next();
      } else if (peek().kind == Tok::Ident && (peek().quoted || !is_reserved(peek().text))) {
        item.alias = peek().text;  // alias implícito: SELECT count(*) total
        next();
      }
      q.select.push_back(std::move(item));
    } while (accept(Tok::Comma));

    expect_kw("from", "tras la lista de SELECT");
    const Token& s = peek();
    if (s.kind != Tok::Ident) fail_at(s, "se esperaba el nombre del stream tras FROM");
    q.from = s.text;
    q.from_pos = s.pos;
    next();

    bool seen_where = false, seen_group = false, seen_window = false, seen_having = false, seen_emit = false,
         seen_alert = false, seen_order = false, seen_limit = false, seen_lateness = false;
    auto once = [&](bool& flag, const Token& at, const char* name) {
      if (flag) fail_at(at, cat("la cláusula ", name, " aparece dos veces"));
      flag = true;
    };

    while (true) {
      const Token& t = peek();
      if (t.kind == Tok::End) break;
      if (t.kind == Tok::Semicolon) {
        next();
        if (peek().kind != Tok::End) fail_at(peek(), "texto después de ';'");
        break;
      }
      if (is_kw(t, "where")) {
        once(seen_where, t, "WHERE");
        next();
        q.where = expr();
      } else if (is_kw(t, "group")) {
        once(seen_group, t, "GROUP BY");
        next();
        expect_kw("by", "tras GROUP");
        do q.group_by.push_back(expr());
        while (accept(Tok::Comma));
      } else if (is_kw(t, "window")) {
        once(seen_window, t, "WINDOW");
        next();
        q.window.present = true;
        q.window.pos = t.pos;
        if (accept_kw("tumbling")) {
          q.window.type = WindowType::Tumbling;
          q.window.size_ms = expect_duration("tras TUMBLING");
          q.window.advance_ms = q.window.size_ms;
        } else if (accept_kw("hopping") || accept_kw("sliding")) {
          q.window.type = WindowType::Hopping;
          q.window.size_ms = expect_duration("con el tamaño de la ventana");
          expect_kw("every", "en una ventana HOPPING (WINDOW HOPPING 1m EVERY 10s)");
          q.window.advance_ms = expect_duration("tras EVERY");
        } else {
          fail_at(peek(), cat("se esperaba TUMBLING o HOPPING tras WINDOW y se encontró ", describe(peek())));
        }
        if (at_kw("lateness") || at_kw("allowed")) {
          once(seen_lateness, peek(), "LATENESS");
          if (accept_kw("allowed")) {
            expect_kw("lateness", "tras ALLOWED");
          } else {
            next();
          }
          q.window.lateness_ms = expect_duration("tras LATENESS");
        }
      } else if (is_kw(t, "lateness")) {
        once(seen_lateness, t, "LATENESS");
        next();
        q.window.lateness_ms = expect_duration("tras LATENESS");
      } else if (is_kw(t, "having")) {
        once(seen_having, t, "HAVING");
        next();
        q.having = expr();
      } else if (is_kw(t, "emit")) {
        once(seen_emit, t, "EMIT");
        next();
        q.emit_pos = t.pos;
        if (accept_kw("final")) {
          q.emit_every_ms = 0;
        } else {
          expect_kw("every", "tras EMIT (EMIT EVERY 1s o EMIT FINAL)");
          q.emit_every_ms = expect_duration("tras EMIT EVERY");
        }
      } else if (is_kw(t, "alert")) {
        once(seen_alert, t, "ALERT");
        next();
        expect_kw("if", "tras ALERT (ALERT IF <condición>)");
        const size_t start = peek().pos.offset;
        q.alert = expr();
        q.alert_source = std::string(trim(src_.substr(start, last_end_ - start)));
        if (accept_kw("for")) {
          q.alert_for = static_cast<uint32_t>(expect_integer("tras FOR", 1, 1000));
          if (!accept_kw("windows") && !accept_kw("window") && !accept_kw("ventanas")) accept_kw("ventana");
        }
      } else if (is_kw(t, "order")) {
        once(seen_order, t, "ORDER BY");
        next();
        expect_kw("by", "tras ORDER");
        do {
          OrderItem item;
          item.expr = expr();
          if (accept_kw("desc"))
            item.desc = true;
          else
            accept_kw("asc");
          q.order_by.push_back(std::move(item));
        } while (accept(Tok::Comma));
      } else if (is_kw(t, "limit")) {
        once(seen_limit, t, "LIMIT");
        next();
        q.limit_pos = peek().pos;
        q.limit = expect_integer("tras LIMIT", 1, 100000);
      } else {
        fail_at(t, cat("se esperaba WHERE, GROUP BY, WINDOW, HAVING, ORDER BY, LIMIT, EMIT o ALERT y se encontró ",
                       describe(t)));
      }
    }
  }

  // --------------------------------------------------------------- expresiones
  ExprPtr expr() { return or_expr(); }

  ExprPtr finish(ExprPtr e) {
    e->end_offset = last_end_;
    return e;
  }

  ExprPtr binary(Op op, ExprPtr l, ExprPtr r, SourcePos pos) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->op = op;
    e->pos = pos;
    e->args.push_back(std::move(l));
    e->args.push_back(std::move(r));
    return finish(std::move(e));
  }

  ExprPtr or_expr() {
    ExprPtr l = and_expr();
    while (at_kw("or")) {
      next();
      const SourcePos p = l->pos;
      l = binary(Op::Or, std::move(l), and_expr(), p);
    }
    return l;
  }

  ExprPtr and_expr() {
    ExprPtr l = not_expr();
    while (at_kw("and")) {
      next();
      const SourcePos p = l->pos;
      l = binary(Op::And, std::move(l), not_expr(), p);
    }
    return l;
  }

  ExprPtr not_expr() {
    if (at_kw("not")) {
      const Token& t = next();
      auto e = make(ExprKind::Unary, t);
      e->op = Op::Not;
      e->args.push_back(not_expr());
      return finish(std::move(e));
    }
    return cmp_expr();
  }

  ExprPtr cmp_expr() {
    ExprPtr l = add_expr();
    const Token& t = peek();
    Op op = Op::None;
    switch (t.kind) {
      case Tok::Eq: op = Op::Eq; break;
      case Tok::Ne: op = Op::Ne; break;
      case Tok::Lt: op = Op::Lt; break;
      case Tok::Le: op = Op::Le; break;
      case Tok::Gt: op = Op::Gt; break;
      case Tok::Ge: op = Op::Ge; break;
      default: break;
    }
    if (op != Op::None) {
      next();
      const SourcePos p = l->pos;
      l = binary(op, std::move(l), add_expr(), p);
      return l;
    }
    // Formas con palabra clave, opcionalmente negadas con NOT.
    bool negated = false;
    if (at_kw("not") && (is_kw(peek(1), "in") || is_kw(peek(1), "like") || is_kw(peek(1), "ilike") ||
                         is_kw(peek(1), "between"))) {
      next();
      negated = true;
    }
    if (accept_kw("in")) {
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::In;
      e->pos = l->pos;
      e->negated = negated;
      e->args.push_back(std::move(l));
      expect(Tok::LParen, "tras IN");
      do e->args.push_back(add_expr());
      while (accept(Tok::Comma));
      expect(Tok::RParen, "al cerrar la lista de IN");
      return finish(std::move(e));
    }
    if (at_kw("like") || at_kw("ilike")) {
      const bool ci = at_kw("ilike");
      next();
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Like;
      e->pos = l->pos;
      e->negated = negated;
      e->case_insensitive = ci;
      e->args.push_back(std::move(l));
      const Token& pat = peek();
      if (pat.kind != Tok::String) fail_at(pat, "LIKE necesita un patrón entre comillas simples, p. ej. '/api/%'");
      e->args.push_back(add_expr());
      return finish(std::move(e));
    }
    if (accept_kw("between")) {
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Between;
      e->pos = l->pos;
      e->negated = negated;
      e->args.push_back(std::move(l));
      e->args.push_back(add_expr());
      expect_kw("and", "en BETWEEN a AND b");
      e->args.push_back(add_expr());
      return finish(std::move(e));
    }
    if (negated) fail_at(peek(), "se esperaba IN, LIKE o BETWEEN tras NOT");
    if (accept_kw("is")) {
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::IsNull;
      e->pos = l->pos;
      e->negated = accept_kw("not");
      expect_kw("null", "tras IS / IS NOT");
      e->args.push_back(std::move(l));
      return finish(std::move(e));
    }
    return l;
  }

  ExprPtr add_expr() {
    ExprPtr l = mul_expr();
    while (true) {
      const Tok k = peek().kind;
      Op op = k == Tok::Plus ? Op::Add : k == Tok::Minus ? Op::Sub : k == Tok::Concat ? Op::Concat : Op::None;
      if (op == Op::None) return l;
      next();
      const SourcePos p = l->pos;
      l = binary(op, std::move(l), mul_expr(), p);
    }
  }

  ExprPtr mul_expr() {
    ExprPtr l = unary_expr();
    while (true) {
      const Tok k = peek().kind;
      Op op = k == Tok::Star ? Op::Mul : k == Tok::Slash ? Op::Div : k == Tok::Percent ? Op::Mod : Op::None;
      if (op == Op::None) return l;
      next();
      const SourcePos p = l->pos;
      l = binary(op, std::move(l), unary_expr(), p);
    }
  }

  ExprPtr unary_expr() {
    if (peek().kind == Tok::Minus) {
      const Token& t = next();
      auto e = make(ExprKind::Unary, t);
      e->op = Op::Neg;
      e->args.push_back(unary_expr());
      return finish(std::move(e));
    }
    if (peek().kind == Tok::Plus) {
      next();
      return unary_expr();
    }
    return primary();
  }

  ExprPtr primary() {
    const Token& t = peek();
    switch (t.kind) {
      case Tok::Number: {
        next();
        auto e = make(ExprKind::Number, t);
        e->number = t.number;
        return finish(std::move(e));
      }
      case Tok::String: {
        next();
        auto e = make(ExprKind::String, t);
        e->text = t.text;
        return finish(std::move(e));
      }
      case Tok::Duration: {
        // Una duración en una expresión vale sus milisegundos: `latency_ms > 2s`.
        next();
        auto e = make(ExprKind::Number, t);
        e->number = static_cast<double>(t.duration_ms);
        return finish(std::move(e));
      }
      case Tok::LParen: {
        next();
        ExprPtr e = expr();
        expect(Tok::RParen, "para cerrar el paréntesis");
        e->end_offset = last_end_;
        return e;
      }
      case Tok::Ident: return ident_expr();
      case Tok::Star: fail_at(t, "'*' solo se admite dentro de count(*)");
      default: fail_at(t, cat("se esperaba una expresión y se encontró ", describe(t)));
    }
  }

  ExprPtr ident_expr() {
    const Token& t = peek();
    if (!t.quoted) {
      if (iequals(t.text, "true") || iequals(t.text, "false")) {
        next();
        auto e = make(ExprKind::Bool, t);
        e->bool_value = iequals(t.text, "true");
        return finish(std::move(e));
      }
      if (iequals(t.text, "null")) {
        next();
        return finish(make(ExprKind::Null, t));
      }
      if (iequals(t.text, "case")) return case_expr();
      if (peek(1).kind == Tok::LParen) return call_expr();
      if (is_reserved(t.text))
        fail_at(t, cat("se esperaba una expresión y se encontró la palabra reservada '", t.text, "'"));
    }
    // Ruta de campo: a.b.c (segmentos numéricos para índices de arrays).
    next();
    auto e = make(ExprKind::Field, t);
    e->text = t.text;
    while (peek().kind == Tok::Dot) {
      next();
      const Token& seg = peek();
      if (seg.kind == Tok::Ident) {
        e->text += '.';
        e->text += seg.text;
        next();
      } else if (seg.kind == Tok::Number && seg.number == std::floor(seg.number) && seg.number >= 0) {
        e->text += '.';
        e->text += seg.text;
        next();
      } else {
        fail_at(seg, "se esperaba un nombre de campo tras '.'");
      }
    }
    return finish(std::move(e));
  }

  ExprPtr call_expr() {
    const Token& name = next();
    auto e = make(ExprKind::Call, name);
    e->text = to_lower_ascii(name.text);
    expect(Tok::LParen);
    if (accept(Tok::RParen)) return finish(std::move(e));
    if (peek().kind == Tok::Star) {
      const Token& st = next();
      e->args.push_back(finish(make(ExprKind::Star, st)));
      expect(Tok::RParen, "tras '*'");
      return finish(std::move(e));
    }
    if (e->text == "count" && at_kw("distinct")) {
      next();
      e->text = "distinct";
    }
    do e->args.push_back(expr());
    while (accept(Tok::Comma));
    expect(Tok::RParen, cat("para cerrar la llamada a ", e->text, "()").c_str());
    return finish(std::move(e));
  }

  ExprPtr case_expr() {
    const Token& t = next();  // CASE
    auto e = make(ExprKind::Case, t);
    if (!at_kw("when")) fail_at(peek(), "se esperaba WHEN tras CASE");
    while (accept_kw("when")) {
      e->args.push_back(expr());
      expect_kw("then", "tras la condición de WHEN");
      e->args.push_back(expr());
    }
    if (accept_kw("else")) {
      e->args.push_back(expr());
      e->has_else = true;
    }
    expect_kw("end", "para cerrar CASE");
    return finish(std::move(e));
  }

  std::string_view src_;
  std::vector<Token> toks_;
  Diagnostic& err_;
  size_t pos_ = 0;
  size_t last_end_ = 0;
};

}  // namespace

bool parse_query(std::string_view src, QueryAst& out, Diagnostic& err) {
  std::vector<Token> toks;
  if (!tokenize(src, toks, err)) return false;
  Parser p(src, std::move(toks), err);
  return p.query(out);
}

bool parse_expression(std::string_view src, ExprPtr& out, Diagnostic& err) {
  std::vector<Token> toks;
  if (!tokenize(src, toks, err)) return false;
  Parser p(src, std::move(toks), err);
  return p.expression_only(out);
}

// ----------------------------------------------------------------------------
// Igualdad estructural y forma canónica
// ----------------------------------------------------------------------------

bool same_expr(const Expr& a, const Expr& b) noexcept {
  if (a.kind != b.kind || a.op != b.op || a.negated != b.negated || a.case_insensitive != b.case_insensitive ||
      a.has_else != b.has_else || a.args.size() != b.args.size())
    return false;
  switch (a.kind) {
    case ExprKind::Bool:
      if (a.bool_value != b.bool_value) return false;
      break;
    case ExprKind::Number:
      if (a.number != b.number) return false;
      break;
    case ExprKind::String:
    case ExprKind::Field:
    case ExprKind::Call:
      if (a.text != b.text) return false;
      break;
    default: break;
  }
  for (size_t i = 0; i < a.args.size(); ++i)
    if (!same_expr(*a.args[i], *b.args[i])) return false;
  return true;
}

namespace {

const char* op_text(Op op) {
  switch (op) {
    case Op::Neg: return "-";
    case Op::Not: return "NOT ";
    case Op::Add: return " + ";
    case Op::Sub: return " - ";
    case Op::Mul: return " * ";
    case Op::Div: return " / ";
    case Op::Mod: return " % ";
    case Op::Concat: return " || ";
    case Op::Eq: return " = ";
    case Op::Ne: return " != ";
    case Op::Lt: return " < ";
    case Op::Le: return " <= ";
    case Op::Gt: return " > ";
    case Op::Ge: return " >= ";
    case Op::And: return " AND ";
    case Op::Or: return " OR ";
    default: return "?";
  }
}

void canon(const Expr& e, std::string& out) {
  switch (e.kind) {
    case ExprKind::Null: out += "NULL"; break;
    case ExprKind::Bool: out += e.bool_value ? "TRUE" : "FALSE"; break;
    case ExprKind::Number: append_double(out, e.number, false); break;
    case ExprKind::String:
      out += '\'';
      for (char c : e.text) {
        if (c == '\'') out += '\'';
        out += c;
      }
      out += '\'';
      break;
    case ExprKind::Field: out += e.text; break;
    case ExprKind::Star: out += '*'; break;
    case ExprKind::Call:
      out += e.text;
      out += '(';
      for (size_t i = 0; i < e.args.size(); ++i) {
        if (i) out += ", ";
        canon(*e.args[i], out);
      }
      out += ')';
      break;
    case ExprKind::Unary:
      out += op_text(e.op);
      out += '(';
      canon(*e.args[0], out);
      out += ')';
      break;
    case ExprKind::Binary:
      out += '(';
      canon(*e.args[0], out);
      out += op_text(e.op);
      canon(*e.args[1], out);
      out += ')';
      break;
    case ExprKind::In:
      canon(*e.args[0], out);
      out += e.negated ? " NOT IN (" : " IN (";
      for (size_t i = 1; i < e.args.size(); ++i) {
        if (i > 1) out += ", ";
        canon(*e.args[i], out);
      }
      out += ')';
      break;
    case ExprKind::Between:
      canon(*e.args[0], out);
      out += e.negated ? " NOT BETWEEN " : " BETWEEN ";
      canon(*e.args[1], out);
      out += " AND ";
      canon(*e.args[2], out);
      break;
    case ExprKind::Like:
      canon(*e.args[0], out);
      out += e.negated ? " NOT " : " ";
      out += e.case_insensitive ? "ILIKE " : "LIKE ";
      canon(*e.args[1], out);
      break;
    case ExprKind::IsNull:
      canon(*e.args[0], out);
      out += e.negated ? " IS NOT NULL" : " IS NULL";
      break;
    case ExprKind::Case: {
      out += "CASE";
      const size_t pairs = (e.args.size() - (e.has_else ? 1 : 0)) / 2;
      for (size_t i = 0; i < pairs; ++i) {
        out += " WHEN ";
        canon(*e.args[2 * i], out);
        out += " THEN ";
        canon(*e.args[2 * i + 1], out);
      }
      if (e.has_else) {
        out += " ELSE ";
        canon(*e.args.back(), out);
      }
      out += " END";
      break;
    }
  }
}

}  // namespace

std::string canonical(const Expr& e) {
  std::string s;
  canon(e, s);
  return s;
}

}  // namespace zr::zql
