// Analizador léxico de ZQL.
//
// Las palabras clave no se distinguen aquí: el lexer emite identificadores y
// el parser decide por contexto (así `if(...)` puede ser una función y
// `ALERT IF` una cláusula). Reconoce duraciones pegadas a un número ("10s",
// "1h30m", "500ms") como un único token.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zql/diagnostic.hpp"

namespace zr::zql {

enum class Tok : uint8_t {
  End,
  Ident,        // nombre (palabra clave o campo); quoted = true si venía entre "" o ``
  String,       // 'texto'
  Number,       // 123, 1.5, 2e3
  Duration,     // 10s, 1h30m
  LParen,
  RParen,
  Comma,
  Dot,
  Star,
  Plus,
  Minus,
  Slash,
  Percent,
  Concat,  // ||
  Eq,      // = o ==
  Ne,      // != o <>
  Lt,
  Le,
  Gt,
  Ge,
  Semicolon,
};

struct Token {
  Tok kind = Tok::End;
  std::string text;   // identificador o cadena ya desescapada
  double number = 0;  // Number
  int64_t duration_ms = 0;
  bool quoted = false;
  SourcePos pos;
  size_t end_offset = 0;  // fin del token en el texto fuente
  bool path_part = false; // nombre o índice que puede continuar una ruta (a.b.0)
};

const char* token_name(Tok t) noexcept;

// Tokeniza todo el texto. Devuelve false y rellena `err` ante un error léxico.
bool tokenize(std::string_view src, std::vector<Token>& out, Diagnostic& err);

}  // namespace zr::zql
