// Subcomandos de la línea de comandos.
#pragma once

#include <string>
#include <vector>

namespace zr::app {

int cmd_serve(const std::vector<std::string>& args);
int cmd_check(const std::vector<std::string>& args);
int cmd_loadgen(const std::vector<std::string>& args);
void print_usage();

}  // namespace zr::app
