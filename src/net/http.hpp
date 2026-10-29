// HTTP/1.1: petición, respuesta y parser incremental.
//
// El parser trabaja sobre el búfer de lectura de la conexión y es reentrante
// entre lecturas: guarda desplazamientos (no punteros), así que el búfer puede
// crecer y realojarse entre llamadas. Soporta Content-Length, cuerpos
// "chunked" (curl -T - o productores en streaming), Expect: 100-continue,
// keep-alive y pipelining, con límites estrictos de cabeceras y cuerpo.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zr::net {

struct Header {
  std::string_view name;
  std::string_view value;
};

struct HttpRequest {
  std::string_view method;
  std::string_view target;
  std::string_view path;
  std::string_view query;
  std::string_view version;
  std::vector<Header> headers;
  std::string_view body;
  bool keep_alive = true;
  std::vector<std::pair<std::string, std::string>> params;  // parámetros de ruta ya decodificados
  std::string peer;

  std::string_view header(std::string_view name) const noexcept;
  bool has_query(std::string_view key) const;
  std::string query_param(std::string_view key, std::string_view def = {}) const;
  std::string_view param(std::string_view name) const noexcept;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  static HttpResponse json(int status, std::string body);
  static HttpResponse text(int status, std::string body);
  static HttpResponse error(int status, std::string_view message);
};

const char* status_text(int status) noexcept;

class RequestParser {
 public:
  enum class Status { NeedMore, Done, Error };

  struct Limits {
    size_t max_header_bytes = 16 * 1024;
    size_t max_body_bytes = 64u << 20;
  };

  RequestParser() = default;
  explicit RequestParser(Limits limits) : limits_(limits) {}

  // Intenta completar una petición al principio de `buf`. Con Done, `req`
  // apunta dentro de `buf` (o del cuerpo decodificado interno) y `consumed`
  // es cuántos bytes ocupa la petición.
  Status parse(std::string_view buf, HttpRequest& req, size_t& consumed);

  bool headers_done() const noexcept { return have_headers_; }
  bool expects_continue() const noexcept { return expect_continue_; }
  int error_status() const noexcept { return error_status_; }
  const std::string& error_message() const noexcept { return error_; }
  void reset() noexcept;

 private:
  Status fail(int status, std::string msg);
  bool parse_head(std::string_view head, HttpRequest& req, bool validate);
  Status decode_chunks(std::string_view buf);

  Limits limits_;
  bool have_headers_ = false;
  size_t scanned_ = 0;
  size_t header_len_ = 0;
  size_t content_length_ = 0;
  bool chunked_ = false;
  bool expect_continue_ = false;
  size_t chunk_pos_ = 0;  // siguiente cabecera de trozo (desde el inicio de la petición)
  size_t chunk_end_ = 0;  // fin de la petición chunked una vez completa
  bool chunks_done_ = false;
  std::string chunked_body_;
  int error_status_ = 400;
  std::string error_;
};

}  // namespace zr::net
