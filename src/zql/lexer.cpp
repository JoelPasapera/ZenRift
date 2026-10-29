#include "zql/lexer.hpp"

#include <charconv>

#include "core/strings.hpp"
#include "core/time.hpp"

namespace zr::zql {

std::string Diagnostic::to_string() const {
  return cat("línea ", pos.line, ", columna ", pos.col, ": ", message);
}

SourcePos position_of(std::string_view src, size_t offset) noexcept {
  SourcePos p;
  p.offset = offset;
  for (size_t i = 0; i < offset && i < src.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    if (c == '\n') {
      ++p.line;
      p.col = 1;
    } else if ((c & 0xC0) != 0x80) {
      ++p.col;  // no contar bytes de continuación UTF-8
    }
  }
  return p;
}

const char* token_name(Tok t) noexcept {
  switch (t) {
    case Tok::End: return "fin de la consulta";
    case Tok::Ident: return "identificador";
    case Tok::String: return "cadena";
    case Tok::Number: return "número";
    case Tok::Duration: return "duración";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::Comma: return "','";
    case Tok::Dot: return "'.'";
    case Tok::Star: return "'*'";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Slash: return "'/'";
    case Tok::Percent: return "'%'";
    case Tok::Concat: return "'||'";
    case Tok::Eq: return "'='";
    case Tok::Ne: return "'!='";
    case Tok::Lt: return "'<'";
    case Tok::Le: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::Ge: return "'>='";
    case Tok::Semicolon: return "';'";
  }
  return "?";
}

namespace {

bool is_ident_start(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool is_ident_char(char c) noexcept { return is_ident_start(c) || (c >= '0' && c <= '9'); }

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace

bool tokenize(std::string_view src, std::vector<Token>& out, Diagnostic& err) {
  out.clear();
  size_t i = 0;
  const size_t n = src.size();
  auto fail = [&](size_t at, std::string msg) {
    err.pos = position_of(src, at);
    err.message = std::move(msg);
    return false;
  };
  while (true) {
    // Espacios y comentarios.
    while (i < n) {
      const char c = src[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i;
      } else if (c == '-' && i + 1 < n && src[i + 1] == '-') {
        while (i < n && src[i] != '\n') ++i;
      } else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
        const size_t start = i;
        i += 2;
        while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
        if (i + 1 >= n) return fail(start, "comentario /* sin cerrar");
        i += 2;
      } else {
        break;
      }
    }
    Token t;
    t.pos.offset = i;
    if (i >= n) {
      t.kind = Tok::End;
      t.end_offset = n;
      t.pos = position_of(src, n);
      out.push_back(std::move(t));
      break;
    }
    const char c = src[i];
    // Rutas de campo con índices de array: en "tags.0.name" el '.' pegado a un
    // nombre (o a un índice) es un separador y el número que le sigue es un
    // índice entero, no el decimal ".0" ni "0.1".
    const Token* prev = out.empty() ? nullptr : &out.back();
    const bool after_path = prev && prev->end_offset == i && prev->path_part;
    const bool index_segment = prev && prev->kind == Tok::Dot && prev->end_offset == i && is_digit(c);
    if (c == '.' && after_path) {
      t.kind = Tok::Dot;
      ++i;
    } else if (index_segment) {
      const size_t s = i;
      while (i < n && is_digit(src[i])) ++i;
      t.kind = Tok::Number;
      t.text.assign(src.substr(s, i - s));
      if (!parse_double(t.text, t.number)) return fail(s, cat("índice inválido '", t.text, "'"));
      t.path_part = true;
    } else if (is_ident_start(c)) {
      const size_t s = i;
      while (i < n && is_ident_char(src[i])) ++i;
      t.kind = Tok::Ident;
      t.text.assign(src.substr(s, i - s));
      t.path_part = true;
    } else if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(src[i + 1]))) {
      const size_t s = i;
      while (i < n && is_digit(src[i])) ++i;
      if (i < n && src[i] == '.' && i + 1 < n && is_digit(src[i + 1])) {
        ++i;
        while (i < n && is_digit(src[i])) ++i;
      }
      // Exponente solo si sigue un dígito (para no confundir "1e" con nada).
      if (i < n && (src[i] == 'e' || src[i] == 'E')) {
        size_t j = i + 1;
        if (j < n && (src[j] == '+' || src[j] == '-')) ++j;
        if (j < n && is_digit(src[j])) {
          i = j;
          while (i < n && is_digit(src[i])) ++i;
        }
      }
      if (i < n && is_ident_start(src[i])) {
        // Duración: número + unidades, posiblemente encadenadas (1h30m).
        while (i < n && (is_ident_char(src[i]) || src[i] == '.')) ++i;
        const std::string_view text = src.substr(s, i - s);
        if (!parse_duration(text, t.duration_ms))
          return fail(s, cat("duración inválida '", text, "' (usa ms, s, m, h, d o w)"));
        t.kind = Tok::Duration;
        t.text.assign(text);
      } else {
        t.kind = Tok::Number;
        t.text.assign(src.substr(s, i - s));
        if (!parse_double(t.text, t.number)) return fail(s, cat("número inválido '", t.text, "'"));
      }
    } else if (c == '\'') {
      // Cadena con '' como comilla escapada.
      const size_t s = i++;
      while (true) {
        if (i >= n) return fail(s, "cadena sin cerrar");
        if (src[i] == '\'') {
          if (i + 1 < n && src[i + 1] == '\'') {
            t.text.push_back('\'');
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        t.text.push_back(src[i++]);
      }
      t.kind = Tok::String;
    } else if (c == '"' || c == '`') {
      // Identificador entre comillas: permite guiones, espacios y palabras reservadas.
      const char q = c;
      const size_t s = i++;
      while (true) {
        if (i >= n) return fail(s, "identificador entre comillas sin cerrar");
        if (src[i] == q) {
          if (i + 1 < n && src[i + 1] == q) {
            t.text.push_back(q);
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        t.text.push_back(src[i++]);
      }
      if (t.text.empty()) return fail(s, "identificador vacío");
      t.kind = Tok::Ident;
      t.quoted = true;
      t.path_part = true;
    } else {
      const char d = i + 1 < n ? src[i + 1] : '\0';
      switch (c) {
        case '(': t.kind = Tok::LParen; ++i; break;
        case ')': t.kind = Tok::RParen; ++i; break;
        case ',': t.kind = Tok::Comma; ++i; break;
        case '.': t.kind = Tok::Dot; ++i; break;
        case '*': t.kind = Tok::Star; ++i; break;
        case '+': t.kind = Tok::Plus; ++i; break;
        case '-': t.kind = Tok::Minus; ++i; break;
        case '/': t.kind = Tok::Slash; ++i; break;
        case '%': t.kind = Tok::Percent; ++i; break;
        case ';': t.kind = Tok::Semicolon; ++i; break;
        case '|':
          if (d != '|') return fail(i, "carácter inesperado '|' (¿querías '||'?)");
          t.kind = Tok::Concat;
          i += 2;
          break;
        case '=':
          t.kind = Tok::Eq;
          i += d == '=' ? 2 : 1;
          break;
        case '!':
          if (d != '=') return fail(i, "carácter inesperado '!' (¿querías '!='?)");
          t.kind = Tok::Ne;
          i += 2;
          break;
        case '<':
          if (d == '=') {
            t.kind = Tok::Le;
            i += 2;
          } else if (d == '>') {
            t.kind = Tok::Ne;
            i += 2;
          } else {
            t.kind = Tok::Lt;
            ++i;
          }
          break;
        case '>':
          if (d == '=') {
            t.kind = Tok::Ge;
            i += 2;
          } else {
            t.kind = Tok::Gt;
            ++i;
          }
          break;
        default:
          return fail(i, cat("carácter inesperado '", std::string_view(&src[i], 1), "'"));
      }
    }
    t.end_offset = i;
    t.pos = position_of(src, t.pos.offset);
    out.push_back(std::move(t));
  }
  return true;
}

}  // namespace zr::zql
