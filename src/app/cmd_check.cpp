// zenrift check: compila una consulta ZQL sin servidor y muestra su plan.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "app/commands.hpp"
#include "core/strings.hpp"
#include "zql/plan.hpp"

namespace zr::app {

int cmd_check(const std::vector<std::string>& args) {
  std::string text;
  bool bytecode = false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--bytecode" || args[i] == "-b") {
      bytecode = true;
    } else if (args[i] == "-f" || args[i] == "--file") {
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "error: falta el archivo tras %s\n", args[i].c_str());
        return 2;
      }
      std::ifstream f(args[++i]);
      if (!f) {
        std::fprintf(stderr, "error: no se pudo abrir %s\n", args[i].c_str());
        return 2;
      }
      std::stringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    } else if (args[i] == "-") {
      std::stringstream ss;
      ss << std::cin.rdbuf();
      text = ss.str();
    } else {
      text = args[i];
    }
  }
  if (text.empty()) {
    std::fprintf(stderr, "uso: zenrift check [--bytecode] \"SELECT ...\" | -f consulta.zql | -\n");
    return 2;
  }
  zql::QueryPlan plan;
  zql::Diagnostic err;
  if (!zql::compile_query(text, zql::CompileOptions{}, plan, err)) {
    std::fprintf(stderr, "error: %s\n", err.to_string().c_str());
    // Mostrar la línea con un acento circunflejo bajo la columna del error.
    const auto lines = split(text, '\n');
    if (err.pos.line >= 1 && static_cast<size_t>(err.pos.line) <= lines.size()) {
      const std::string_view line = lines[static_cast<size_t>(err.pos.line - 1)];
      std::fprintf(stderr, "  %.*s\n  %*s^\n", static_cast<int>(line.size()), line.data(), err.pos.col - 1, "");
    }
    return 1;
  }
  std::fputs(plan.explain(bytecode).c_str(), stdout);
  return 0;
}

}  // namespace zr::app
