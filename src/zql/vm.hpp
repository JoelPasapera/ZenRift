// Máquina virtual de pila que ejecuta los programas de ZQL.
//
// La misma VM sirve para los dos ámbitos de evaluación:
//  - evento: WHERE, claves de GROUP BY y argumentos de las agregaciones
//    (lee campos del evento a través de `field_map`);
//  - fila: SELECT, HAVING, ORDER BY y ALERT al cerrar una ventana (lee
//    claves de grupo, resultados de agregaciones y columnas ya calculadas).
#pragma once

#include <cstdint>

#include "core/arena.hpp"
#include "zql/program.hpp"
#include "zql/value.hpp"

namespace zr::zql {

constexpr uint32_t kMaxStack = 64;

struct EvalContext {
  // Ámbito de evento: valor del campo i del plan = fields[field_map[i]]
  // (o fields[i] si field_map es nulo).
  const Value* fields = nullptr;
  const uint32_t* field_map = nullptr;
  // Ámbito de fila.
  const Value* keys = nullptr;
  const Value* aggs = nullptr;
  const Value* cols = nullptr;
  int64_t window_start = 0;
  int64_t window_end = 0;
  // Memoria para resultados de texto intermedios.
  Arena* scratch = nullptr;
};

Value eval(const Program& prog, const EvalContext& ctx);

// Atajo para condiciones: true solo si el resultado es VERDADERO.
inline bool eval_true(const Program& prog, const EvalContext& ctx) { return truth(eval(prog, ctx)) == Tri::True; }

// Operaciones compartidas con el plegado de constantes del compilador.
Value apply_unary(OpCode op, const Value& v);
Value apply_binary(OpCode op, const Value& a, const Value& b, Arena& scratch);

}  // namespace zr::zql
