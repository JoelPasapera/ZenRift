// Programa de bytecode de ZQL: instrucciones de una VM de pila más sus tablas
// de constantes, conjuntos IN y patrones LIKE. Es inmutable tras compilarse,
// así que varios hilos pueden evaluarlo a la vez.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/flat_map.hpp"
#include "zql/value.hpp"

namespace zr::zql {

enum class OpCode : uint8_t {
  Const,          // push consts[a]
  Field,          // push campo a del evento
  Key,            // push clave de grupo a
  Agg,            // push resultado de agregación a
  Col,            // push columna de salida a (alias en HAVING/ALERT/ORDER)
  WinStart,       // push inicio de la ventana (ms)
  WinEnd,         // push fin de la ventana (ms)
  WinSecs,        // push duración de la ventana en segundos
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
  And,            // combina dos operandos con lógica trivalente
  Or,
  JumpIfFalse,    // si la cima es FALSO salta a `a` sin desapilar (cortocircuito de AND)
  JumpIfTrue,     // si la cima es VERDADERO salta a `a` sin desapilar (cortocircuito de OR)
  JumpIfNotTrue,  // desapila; si no es VERDADERO salta a `a` (CASE)
  Jump,
  Pop,
  IsNull,
  IsNotNull,
  InSet,          // desapila x; push x ∈ sets[a]
  Like,           // desapila x; push x LIKE likes[a]
  Call,           // llama a la función a con b argumentos
  Ret,
};

const char* opcode_name(OpCode op) noexcept;

struct Instr {
  OpCode op;
  uint8_t b = 0;
  uint16_t reserved = 0;
  uint32_t a = 0;
};

// Conjunto de constantes para IN, indexado por la codificación canónica.
class ValueSet {
 public:
  ValueSet() = default;
  ValueSet(ValueSet&&) noexcept = default;
  ValueSet& operator=(ValueSet&&) noexcept = default;
  ValueSet(const ValueSet&) = delete;  // el índice apunta a las cadenas propias
  ValueSet& operator=(const ValueSet&) = delete;

  void add(const Value& v);
  void seal();  // construye el índice; llamar tras el último add()
  bool contains(const Value& v) const;
  size_t size() const noexcept { return items_.size(); }

 private:
  std::vector<std::string> items_;
  FlatMap<std::string_view, uint8_t> index_;
};

// Patrón LIKE: '%' cualquier secuencia, '_' un carácter UTF-8.
struct LikePattern {
  std::string pattern;
  bool case_insensitive = false;
  bool matches(std::string_view s) const noexcept;
};

struct Program {
  std::vector<Instr> code;
  std::vector<Value> consts;
  std::vector<std::unique_ptr<std::string>> strings;  // almacén estable de las constantes de texto
  std::vector<ValueSet> sets;
  std::vector<LikePattern> likes;
  uint32_t max_stack = 0;

  Program() = default;
  Program(Program&&) noexcept = default;
  Program& operator=(Program&&) noexcept = default;
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  bool empty() const noexcept { return code.empty(); }

  // Añade una constante (copiando las cadenas al almacén propio).
  uint32_t add_const(const Value& v);
  std::string disassemble() const;
};

}  // namespace zr::zql
