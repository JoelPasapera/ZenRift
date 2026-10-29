// Funciones escalares de ZQL (lower, round, coalesce, split_part...).
//
// Las agregaciones (count, p99, distinct, topk...) NO están aquí: las reconoce
// el compilador y las implementa el motor, porque tienen estado.
#pragma once

#include <cstdint>
#include <string_view>

#include "core/arena.hpp"
#include "zql/value.hpp"

namespace zr::zql {

using FunctionImpl = Value (*)(const Value* args, uint32_t argc, Arena& scratch);

struct FunctionInfo {
  const char* name;
  uint8_t min_args;
  uint8_t max_args;
  bool pure;  // sin efectos ni dependencia del reloj: se puede plegar en compilación
  FunctionImpl impl;
  const char* help;
};

// nullptr si no existe. El índice devuelto por function_id es estable.
const FunctionInfo* find_function(std::string_view name) noexcept;
uint32_t function_id(const FunctionInfo* f) noexcept;
const FunctionInfo& function_at(uint32_t id) noexcept;
size_t function_count() noexcept;

}  // namespace zr::zql
