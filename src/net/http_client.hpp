// Cliente HTTP/1.1 bloqueante con keep-alive, para el generador de carga y
// las herramientas de línea de comandos. Una conexión, peticiones en serie.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "net/socket.hpp"

namespace zr::net {

struct ClientResponse {
  int status = 0;
  std::string body;
};

class HttpClient {
 public:
  HttpClient(std::string host, uint16_t port, std::string auth_token = {});
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  bool request(std::string_view method, std::string_view path, std::string_view body, std::string_view content_type,
               ClientResponse& out, std::string& err);

 private:
  bool ensure_connected(std::string& err);
  bool read_response(ClientResponse& out, std::string& err);
  void disconnect();

  std::string host_;
  uint16_t port_;
  std::string token_;
  socket_t sock_ = kInvalidSocket;
  std::string buf_;
};

// "http://host:port" -> host, port
bool parse_http_url(std::string_view url, std::string& host, uint16_t& port, std::string& err);

}  // namespace zr::net
