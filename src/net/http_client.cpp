#include "net/http_client.hpp"

#include "core/strings.hpp"

namespace zr::net {

bool parse_http_url(std::string_view url, std::string& host, uint16_t& port, std::string& err) {
  if (url.starts_with("http://")) url.remove_prefix(7);
  if (url.starts_with("https://")) {
    err = "https no está soportado; usa http://";
    return false;
  }
  const size_t slash = url.find('/');
  if (slash != std::string_view::npos) url = url.substr(0, slash);
  port = 80;
  const size_t colon = url.rfind(':');
  if (colon != std::string_view::npos && url.find(']') == std::string_view::npos) {
    int64_t p;
    if (!parse_int64(url.substr(colon + 1), p) || p < 1 || p > 65535) {
      err = cat("puerto inválido en '", url, "'");
      return false;
    }
    port = static_cast<uint16_t>(p);
    url = url.substr(0, colon);
  }
  host = std::string(url.empty() ? "127.0.0.1" : url);
  return true;
}

HttpClient::HttpClient(std::string host, uint16_t port, std::string auth_token)
    : host_(std::move(host)), port_(port), token_(std::move(auth_token)) {}

HttpClient::~HttpClient() { disconnect(); }

void HttpClient::disconnect() {
  close_socket(sock_);
  sock_ = kInvalidSocket;
  buf_.clear();
}

bool HttpClient::ensure_connected(std::string& err) {
  if (sock_ != kInvalidSocket) return true;
  sock_ = connect_tcp(host_, port_, err);
  return sock_ != kInvalidSocket;
}

bool HttpClient::request(std::string_view method, std::string_view path, std::string_view body,
                         std::string_view content_type, ClientResponse& out, std::string& err) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensure_connected(err)) return false;
    std::string req;
    req.reserve(256 + body.size());
    append_all(req, method, " ", path, " HTTP/1.1\r\nHost: ", host_, "\r\nUser-Agent: zenrift-cli\r\n");
    if (!token_.empty()) append_all(req, "Authorization: Bearer ", token_, "\r\n");
    if (!body.empty() || method == "POST" || method == "PUT") {
      append_all(req, "Content-Type: ", content_type, "\r\nContent-Length: ", body.size(), "\r\n");
    }
    req += "\r\n";
    req.append(body);
    size_t off = 0;
    bool broken = false;
    while (off < req.size()) {
      const long n = send_some(sock_, req.data() + off, req.size() - off);
      if (n <= 0) {
        broken = true;
        break;
      }
      off += static_cast<size_t>(n);
    }
    if (!broken && read_response(out, err)) return true;
    // Conexión keep-alive cerrada por el servidor: reintentar una vez.
    disconnect();
    if (attempt == 1 && err.empty()) err = "la conexión se cerró";
  }
  return false;
}

bool HttpClient::read_response(ClientResponse& out, std::string& err) {
  buf_.clear();
  size_t header_end = std::string::npos;
  char tmp[64 * 1024];
  while (header_end == std::string::npos) {
    const long n = recv_some(sock_, tmp, sizeof tmp);
    if (n <= 0) {
      err = "la conexión se cerró antes de la respuesta";
      return false;
    }
    buf_.append(tmp, static_cast<size_t>(n));
    header_end = buf_.find("\r\n\r\n");
  }
  const std::string_view head(buf_.data(), header_end);
  if (head.size() < 12 || !head.starts_with("HTTP/1.")) {
    err = "respuesta HTTP inválida";
    return false;
  }
  int64_t status = 0;
  parse_int64(head.substr(9, 3), status);
  out.status = static_cast<int>(status);
  int64_t length = -1;
  bool close_after = false;
  for (auto line : split(head, '\n')) {
    line = trim(line);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string_view name = line.substr(0, colon), value = trim(line.substr(colon + 1));
    if (iequals(name, "Content-Length")) parse_int64(value, length);
    if (iequals(name, "Connection") && iequals(value, "close")) close_after = true;
  }
  if (length < 0) {
    err = "respuesta sin Content-Length";
    return false;
  }
  const size_t total = header_end + 4 + static_cast<size_t>(length);
  while (buf_.size() < total) {
    const long n = recv_some(sock_, tmp, sizeof tmp);
    if (n <= 0) {
      err = "respuesta incompleta";
      return false;
    }
    buf_.append(tmp, static_cast<size_t>(n));
  }
  out.body.assign(buf_, header_end + 4, static_cast<size_t>(length));
  if (close_after) disconnect();
  return true;
}

}  // namespace zr::net
