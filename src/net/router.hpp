// Enrutador de rutas con parámetros: "/v1/streams/{name}/events".
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/http.hpp"
#include "net/server.hpp"

namespace zr::net {

class Router {
 public:
  using Handler = std::function<void(HttpServer&, const ConnHandle&, HttpRequest&)>;

  void add(std::string_view method, std::string_view pattern, Handler h);

  enum class Match { Found, NotFound, MethodNotAllowed };
  // Rellena req.params con los parámetros (decodificados) de la ruta.
  Match match(HttpRequest& req, const Handler*& out, std::string& allowed) const;

 private:
  struct Route {
    std::string method;
    std::vector<std::string> segments;  // "{x}" = parámetro
    Handler handler;
  };
  std::vector<Route> routes_;
};

}  // namespace zr::net
