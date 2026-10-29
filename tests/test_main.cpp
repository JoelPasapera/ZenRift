// Ejecutor de pruebas: `zenrift_tests [filtro]` ejecuta las pruebas cuyo
// nombre contiene el filtro.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "core/log.hpp"
#include "framework.hpp"

namespace zt {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

namespace {
int g_failures = 0;
}

void report_failure(const char* file, int line, const std::string& msg) {
  ++g_failures;
  std::fprintf(stderr, "    FALLO %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace zt

int main(int argc, char** argv) {
  zr::log::set_level(zr::log::Level::Warn);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& tc : zt::registry()) {
    if (filter && !std::strstr(tc.name, filter)) continue;
    ++run;
    const int before = zt::g_failures;
    const auto s = std::chrono::steady_clock::now();
    try {
      tc.fn();
    } catch (const zt::RequireFailed&) {
    } catch (const std::exception& e) {
      zt::report_failure(tc.file, tc.line, std::string("excepción: ") + e.what());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
    const bool ok = zt::g_failures == before;
    if (!ok) ++failed;
    std::fprintf(stderr, "%s %-48s %8.1f ms\n", ok ? "  ok " : "FALLA", tc.name, ms);
  }
  const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "\n%d pruebas, %d fallidas (%.0f ms)\n", run, failed, total);
  return failed == 0 && run > 0 ? 0 : 1;
}
