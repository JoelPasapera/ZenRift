// Mini marco de pruebas, sin dependencias.
//
//   TEST(nombre) { CHECK(a == b); CHECK_EQ(x, 3); REQUIRE(ptr != nullptr); }
//
// CHECK registra el fallo y sigue; REQUIRE aborta la prueba actual.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "core/strings.hpp"

namespace zt {

struct TestCase {
  const char* name;
  void (*fn)();
  const char* file;
  int line;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, void (*fn)(), const char* file, int line) {
    registry().push_back({name, fn, file, line});
  }
};

struct RequireFailed {};

void report_failure(const char* file, int line, const std::string& msg);

template <class A, class B>
std::string describe(const A& a, const B& b) {
  std::string s;
  zr::append_all(s, a);
  s += " vs ";
  zr::append_all(s, b);
  return s;
}

}  // namespace zt

#define ZT_JOIN2(a, b) a##b
#define ZT_JOIN(a, b) ZT_JOIN2(a, b)

#define TEST(name)                                                              \
  static void name();                                                           \
  static ::zt::Registrar ZT_JOIN(zt_reg_, name)(#name, &name, __FILE__, __LINE__); \
  static void name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) ::zt::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define REQUIRE(cond)                                                     \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ::zt::report_failure(__FILE__, __LINE__, "REQUIRE(" #cond ")");     \
      throw ::zt::RequireFailed{};                                        \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                                          \
  do {                                                                                          \
    const auto& zt_a = (a);                                                                     \
    const auto& zt_b = (b);                                                                     \
    if (!(zt_a == zt_b))                                                                        \
      ::zt::report_failure(__FILE__, __LINE__,                                                  \
                           std::string("CHECK_EQ(" #a ", " #b "): ") + ::zt::describe(zt_a, zt_b)); \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                    \
  do {                                                                                           \
    const double zt_a = static_cast<double>(a);                                                  \
    const double zt_b = static_cast<double>(b);                                                  \
    if (!(std::fabs(zt_a - zt_b) <= (eps)))                                                      \
      ::zt::report_failure(__FILE__, __LINE__,                                                   \
                           std::string("CHECK_NEAR(" #a ", " #b "): ") + ::zt::describe(zt_a, zt_b)); \
  } while (0)
