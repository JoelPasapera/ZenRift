// ZenRift: motor de analítica de eventos en tiempo real.
//
//   zenrift serve [--demo] [--port 7070] [--config zenrift.conf] ...
//   zenrift check "SELECT ..."        compila una consulta y muestra su plan
//   zenrift loadgen [--rate N] ...    tráfico sintético contra un servidor
#include <cstdio>
#include <string>
#include <vector>

#include "app/commands.hpp"
#include "app/config.hpp"

namespace zr::app {

void print_usage() {
  std::printf(
      "ZenRift %s: motor de analítica de eventos en tiempo real\n\n"
      "Uso:\n"
      "  zenrift serve [opciones]          servidor HTTP, motor y panel web (por defecto)\n"
      "  zenrift serve --demo              con tráfico sintético y consultas de ejemplo\n"
      "  zenrift check \"SELECT ...\"        compila una consulta ZQL y muestra su plan\n"
      "  zenrift check --bytecode -f q.zql incluye el bytecode generado\n"
      "  zenrift loadgen [--rate N]        genera carga contra un servidor y la mide\n"
      "  zenrift version | help\n\n"
      "Opciones de serve: --config archivo y cualquier ajuste como --clave valor.\n\n%s",
      ZENRIFT_VERSION, settings_help());
}

}  // namespace zr::app

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string cmd = "serve";
  if (!args.empty() && !args[0].starts_with("-")) {
    cmd = args[0];
    args.erase(args.begin());
  }
  if (cmd == "serve") return zr::app::cmd_serve(args);
  if (cmd == "check") return zr::app::cmd_check(args);
  if (cmd == "loadgen") return zr::app::cmd_loadgen(args);
  if (cmd == "version" || cmd == "--version") {
    std::printf("zenrift %s\n", ZENRIFT_VERSION);
    return 0;
  }
  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    zr::app::print_usage();
    return 0;
  }
  std::fprintf(stderr, "comando desconocido '%s'\n\n", cmd.c_str());
  zr::app::print_usage();
  return 2;
}
