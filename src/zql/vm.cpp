#include "zql/vm.hpp"

#include <cmath>
#include <string>

#include "zql/functions.hpp"

namespace zr::zql {

namespace {

inline Value tri_value(Tri t) noexcept { return t == Tri::Unknown ? Value::null() : Value::boolean(t == Tri::True); }

inline Tri tri_not(Tri t) noexcept { return t == Tri::Unknown ? Tri::Unknown : (t == Tri::True ? Tri::False : Tri::True); }

inline Tri tri_and(Tri a, Tri b) noexcept {
  if (a == Tri::False || b == Tri::False) return Tri::False;
  if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
  return Tri::True;
}

inline Tri tri_or(Tri a, Tri b) noexcept {
  if (a == Tri::True || b == Tri::True) return Tri::True;
  if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
  return Tri::False;
}

inline Value arith(OpCode op, double x, double y) noexcept {
  double r;
  switch (op) {
    case OpCode::Add: r = x + y; break;
    case OpCode::Sub: r = x - y; break;
    case OpCode::Mul: r = x * y; break;
    case OpCode::Div:
      if (y == 0) return Value::null();
      r = x / y;
      break;
    case OpCode::Mod:
      if (y == 0) return Value::null();
      r = std::fmod(x, y);
      break;
    default: return Value::null();
  }
  return std::isfinite(r) ? Value::number(r) : Value::null();
}

}  // namespace

Value apply_unary(OpCode op, const Value& v) {
  if (op == OpCode::Neg) {
    double x;
    return to_number(v, x) ? Value::number(-x) : Value::null();
  }
  if (op == OpCode::Not) return tri_value(tri_not(truth(v)));
  if (op == OpCode::IsNull) return Value::boolean(v.is_null());
  if (op == OpCode::IsNotNull) return Value::boolean(!v.is_null());
  return Value::null();
}

Value apply_binary(OpCode op, const Value& a, const Value& b, Arena& scratch) {
  switch (op) {
    case OpCode::Add:
    case OpCode::Sub:
    case OpCode::Mul:
    case OpCode::Div:
    case OpCode::Mod: {
      double x, y;
      if (!to_number(a, x) || !to_number(b, y)) return Value::null();
      return arith(op, x, y);
    }
    case OpCode::Concat: {
      if (a.is_null() || b.is_null()) return Value::null();
      std::string tmp;
      append_text(tmp, a);
      append_text(tmp, b);
      return Value::string(scratch.copy(tmp));
    }
    case OpCode::Eq: return tri_value(equals(a, b));
    case OpCode::Ne: {
      const Tri t = equals(a, b);
      return tri_value(tri_not(t));
    }
    case OpCode::Lt:
    case OpCode::Le:
    case OpCode::Gt:
    case OpCode::Ge: {
      bool ok;
      const int c = compare(a, b, ok);
      if (!ok) return Value::null();
      const bool r = op == OpCode::Lt ? c < 0 : op == OpCode::Le ? c <= 0 : op == OpCode::Gt ? c > 0 : c >= 0;
      return Value::boolean(r);
    }
    case OpCode::And: return tri_value(tri_and(truth(a), truth(b)));
    case OpCode::Or: return tri_value(tri_or(truth(a), truth(b)));
    default: return Value::null();
  }
}

Value eval(const Program& prog, const EvalContext& ctx) {
  // Pila sin inicializar: Value es un tipo de vida implícita y cada ranura se
  // escribe antes de leerse (el compilador verifica la profundidad máxima).
  alignas(Value) unsigned char raw[sizeof(Value) * kMaxStack];
  Value* stack = reinterpret_cast<Value*>(raw);
  uint32_t sp = 0;
  const Instr* code = prog.code.data();
  const uint32_t n = static_cast<uint32_t>(prog.code.size());
  Arena* scratch = ctx.scratch;
  thread_local Arena fallback(4096);
  if (!scratch) {
    fallback.reset();
    scratch = &fallback;
  }
  uint32_t pc = 0;
  while (pc < n) {
    const Instr& in = code[pc++];
    switch (in.op) {
      case OpCode::Const: stack[sp++] = prog.consts[in.a]; break;
      case OpCode::Field:
        stack[sp++] = ctx.fields ? ctx.fields[ctx.field_map ? ctx.field_map[in.a] : in.a] : Value::null();
        break;
      case OpCode::Key: stack[sp++] = ctx.keys ? ctx.keys[in.a] : Value::null(); break;
      case OpCode::Agg: stack[sp++] = ctx.aggs ? ctx.aggs[in.a] : Value::null(); break;
      case OpCode::Col: stack[sp++] = ctx.cols ? ctx.cols[in.a] : Value::null(); break;
      case OpCode::WinStart: stack[sp++] = Value::number(static_cast<double>(ctx.window_start)); break;
      case OpCode::WinEnd: stack[sp++] = Value::number(static_cast<double>(ctx.window_end)); break;
      case OpCode::WinSecs:
        stack[sp++] = Value::number(static_cast<double>(ctx.window_end - ctx.window_start) / 1000.0);
        break;
      case OpCode::Neg:
      case OpCode::Not:
      case OpCode::IsNull:
      case OpCode::IsNotNull: stack[sp - 1] = apply_unary(in.op, stack[sp - 1]); break;
      case OpCode::Add:
      case OpCode::Sub:
      case OpCode::Mul:
      case OpCode::Div:
      case OpCode::Mod: {
        // Ruta rápida numérica sin pasar por apply_binary.
        const Value& a = stack[sp - 2];
        const Value& b = stack[sp - 1];
        if (a.type == VType::Num && b.type == VType::Num)
          stack[sp - 2] = arith(in.op, a.num, b.num);
        else
          stack[sp - 2] = apply_binary(in.op, a, b, *scratch);
        --sp;
        break;
      }
      case OpCode::Lt:
      case OpCode::Le:
      case OpCode::Gt:
      case OpCode::Ge: {
        const Value& a = stack[sp - 2];
        const Value& b = stack[sp - 1];
        if (a.type == VType::Num && b.type == VType::Num) {
          const double x = a.num, y = b.num;
          bool r;
          switch (in.op) {
            case OpCode::Lt: r = x < y; break;
            case OpCode::Le: r = x <= y; break;
            case OpCode::Gt: r = x > y; break;
            default: r = x >= y; break;
          }
          stack[sp - 2] = (x != x || y != y) ? Value::null() : Value::boolean(r);
        } else {
          stack[sp - 2] = apply_binary(in.op, a, b, *scratch);
        }
        --sp;
        break;
      }
      case OpCode::Concat:
      case OpCode::Eq:
      case OpCode::Ne:
      case OpCode::And:
      case OpCode::Or:
        stack[sp - 2] = apply_binary(in.op, stack[sp - 2], stack[sp - 1], *scratch);
        --sp;
        break;
      case OpCode::JumpIfFalse:
        if (truth(stack[sp - 1]) == Tri::False) pc = in.a;
        break;
      case OpCode::JumpIfTrue:
        if (truth(stack[sp - 1]) == Tri::True) pc = in.a;
        break;
      case OpCode::JumpIfNotTrue:
        if (truth(stack[--sp]) != Tri::True) pc = in.a;
        break;
      case OpCode::Jump: pc = in.a; break;
      case OpCode::Pop: --sp; break;
      case OpCode::InSet: {
        Value& v = stack[sp - 1];
        v = v.is_null() ? Value::null() : Value::boolean(prog.sets[in.a].contains(v));
        break;
      }
      case OpCode::Like: {
        Value& v = stack[sp - 1];
        v = v.type == VType::Str ? Value::boolean(prog.likes[in.a].matches(v.str())) : Value::null();
        break;
      }
      case OpCode::Call: {
        const uint32_t argc = in.b;
        const FunctionInfo& f = function_at(in.a);
        const Value r = f.impl(&stack[sp - argc], argc, *scratch);
        sp -= argc;
        stack[sp++] = r;
        break;
      }
      case OpCode::Ret: return sp ? stack[sp - 1] : Value::null();
    }
  }
  return sp ? stack[sp - 1] : Value::null();
}

}  // namespace zr::zql
