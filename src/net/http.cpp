#include "net/http.hpp"

#include "core/strings.hpp"
#include "json/writer.hpp"

namespace zr::net {

// ---------------------------------------------------------------- petición

std::string_view HttpRequest::header(std::string_view name) const noexcept {
  for (const auto& h : headers)
    if (iequals(h.name, name)) return h.value;
  return {};
}

namespace {

// Recorre los pares clave=valor de la query string.
template <class F>
void for_each_query(std::string_view q, F&& f) {
  size_t start = 0;
  while (start <= q.size()) {
    size_t amp = q.find('&', start);
    if (amp == std::string_view::npos) amp = q.size();
    const std::string_view pair = q.substr(start, amp - start);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (f(pair.substr(0, eq), eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1))) return;
    }
    start = amp + 1;
  }
}

}  // namespace

bool HttpRequest::has_query(std::string_view key) const {
  bool found = false;
  for_each_query(query, [&](std::string_view k, std::string_view) {
    found = url_decode(k) == key;
    return found;
  });
  return found;
}

std::string HttpRequest::query_param(std::string_view key, std::string_view def) const {
  std::string out(def);
  for_each_query(query, [&](std::string_view k, std::string_view v) {
    if (url_decode(k) != key) return false;
    out = url_decode(v);
    return true;
  });
  return out;
}

std::string_view HttpRequest::param(std::string_view name) const noexcept {
  for (const auto& [k, v] : params)
    if (k == name) return v;
  return {};
}

// ---------------------------------------------------------------- respuesta

HttpResponse HttpResponse::json(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

HttpResponse HttpResponse::text(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.content_type = "text/plain; charset=utf-8";
  r.body = std::move(body);
  return r;
}

HttpResponse HttpResponse::error(int status, std::string_view message) {
  HttpResponse r;
  r.status = status;
  json::Writer(r.body).begin_object().kv("error", message).end_object();
  return r;
}

const char* status_text(int status) noexcept {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    default: return "Unknown";
  }
}

// ---------------------------------------------------------------- parser

namespace {

bool is_token_char(char c) noexcept {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~': return true;
    default: return false;
  }
}

std::string_view trim_ows(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool contains_token(std::string_view list, std::string_view token) {
  for (auto part : split(list, ',')) {
    part = trim_ows(part);
    if (iequals(part, token)) return true;
  }
  return false;
}

}  // namespace

void RequestParser::reset() noexcept {
  have_headers_ = false;
  scanned_ = 0;
  header_len_ = 0;
  content_length_ = 0;
  chunked_ = false;
  expect_continue_ = false;
  chunk_pos_ = 0;
  chunk_end_ = 0;
  chunks_done_ = false;
  chunked_body_.clear();
}

RequestParser::Status RequestParser::fail(int status, std::string msg) {
  error_status_ = status;
  error_ = std::move(msg);
  return Status::Error;
}

bool RequestParser::parse_head(std::string_view head, HttpRequest& req, bool validate) {
  req.headers.clear();
  req.params.clear();
  // Línea de petición.
  const size_t eol = head.find("\r\n");
  const std::string_view line = head.substr(0, eol);
  const size_t sp1 = line.find(' ');
  const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos) {
    error_status_ = 400;
    error_ = "línea de petición mal formada";
    return false;
  }
  req.method = line.substr(0, sp1);
  req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  req.version = line.substr(sp2 + 1);
  if (validate) {
    for (char c : req.method)
      if (!is_token_char(c)) {
        error_status_ = 400;
        error_ = "método inválido";
        return false;
      }
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") {
      error_status_ = 505;
      error_ = "solo se admite HTTP/1.0 y HTTP/1.1";
      return false;
    }
    if (req.target.empty() || (req.target[0] != '/' && req.target != "*")) {
      error_status_ = 400;
      error_ = "destino de la petición inválido";
      return false;
    }
    if (req.target.size() > 8192) {
      error_status_ = 414;
      error_ = "URI demasiado larga";
      return false;
    }
  }
  const size_t q = req.target.find('?');
  req.path = req.target.substr(0, q);
  req.query = q == std::string_view::npos ? std::string_view() : req.target.substr(q + 1);
  // Cabeceras.
  size_t pos = eol + 2;
  while (pos < head.size()) {
    const size_t e = head.find("\r\n", pos);
    const std::string_view h = head.substr(pos, e - pos);
    pos = e + 2;
    if (h.empty()) break;
    if (validate && (h[0] == ' ' || h[0] == '\t')) {
      error_status_ = 400;
      error_ = "plegado de cabeceras obsoleto no admitido";
      return false;
    }
    const size_t colon = h.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      error_status_ = 400;
      error_ = "cabecera mal formada";
      return false;
    }
    Header hd{h.substr(0, colon), trim_ows(h.substr(colon + 1))};
    if (validate)
      for (char c : hd.name)
        if (!is_token_char(c)) {
          error_status_ = 400;
          error_ = "nombre de cabecera inválido";
          return false;
        }
    req.headers.push_back(hd);
  }
  if (req.headers.size() > 100 && validate) {
    error_status_ = 431;
    error_ = "demasiadas cabeceras";
    return false;
  }
  const std::string_view conn = req.header("Connection");
  if (req.version == "HTTP/1.0")
    req.keep_alive = contains_token(conn, "keep-alive");
  else
    req.keep_alive = !contains_token(conn, "close");
  return true;
}

RequestParser::Status RequestParser::decode_chunks(std::string_view buf) {
  // buf empieza en el inicio de la petición; los trozos tras la cabecera.
  size_t pos = chunk_pos_;
  while (true) {
    const size_t eol = buf.find("\r\n", pos);
    if (eol == std::string_view::npos) {
      if (buf.size() - pos > 1024) return fail(400, "línea de tamaño de trozo demasiado larga");
      chunk_pos_ = pos;
      return Status::NeedMore;
    }
    std::string_view size_line = buf.substr(pos, eol - pos);
    const size_t semi = size_line.find(';');
    if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);
    size_line = trim_ows(size_line);
    if (size_line.empty() || size_line.size() > 15) return fail(400, "tamaño de trozo inválido");
    uint64_t size = 0;
    for (char c : size_line) {
      int v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else return fail(400, "tamaño de trozo inválido");
      size = size * 16 + static_cast<uint64_t>(v);
    }
    if (size == 0) {
      // Trailers opcionales hasta una línea vacía.
      size_t t = eol + 2;
      while (true) {
        const size_t te = buf.find("\r\n", t);
        if (te == std::string_view::npos) {
          chunk_pos_ = pos;
          return Status::NeedMore;
        }
        if (te == t) {
          chunk_end_ = te + 2;
          chunks_done_ = true;
          return Status::Done;
        }
        t = te + 2;
      }
    }
    if (chunked_body_.size() + size > limits_.max_body_bytes) return fail(413, "cuerpo demasiado grande");
    const size_t data = eol + 2;
    if (buf.size() < data + size + 2) {
      chunk_pos_ = pos;
      return Status::NeedMore;
    }
    if (buf.substr(data + size, 2) != "\r\n") return fail(400, "trozo sin CRLF final");
    chunked_body_.append(buf.data() + data, size);
    pos = data + size + 2;
  }
}

RequestParser::Status RequestParser::parse(std::string_view buf, HttpRequest& req, size_t& consumed) {
  if (!have_headers_) {
    const size_t from = scanned_ > 3 ? scanned_ - 3 : 0;
    const size_t end = buf.find("\r\n\r\n", from);
    if (end == std::string_view::npos) {
      if (buf.size() > limits_.max_header_bytes) return fail(431, "cabeceras demasiado grandes");
      scanned_ = buf.size();
      return Status::NeedMore;
    }
    if (end + 4 > limits_.max_header_bytes) return fail(431, "cabeceras demasiado grandes");
    header_len_ = end + 4;
    HttpRequest tmp;
    if (!parse_head(buf.substr(0, header_len_), tmp, true)) return Status::Error;
    const std::string_view te = tmp.header("Transfer-Encoding");
    const std::string_view cl = tmp.header("Content-Length");
    if (!te.empty()) {
      if (!cl.empty()) return fail(400, "Transfer-Encoding y Content-Length a la vez");
      if (!iequals(trim_ows(te), "chunked")) return fail(501, "Transfer-Encoding no soportado (solo chunked)");
      chunked_ = true;
      chunk_pos_ = header_len_;
      chunked_body_.clear();  // el cuerpo de la petición anterior ya se consumió
    } else if (!cl.empty()) {
      int64_t n;
      if (!parse_int64(cl, n) || n < 0) return fail(400, "Content-Length inválido");
      for (const auto& h : tmp.headers)
        if (iequals(h.name, "Content-Length") && h.value != cl) return fail(400, "Content-Length duplicado");
      if (static_cast<uint64_t>(n) > limits_.max_body_bytes) return fail(413, "cuerpo demasiado grande");
      content_length_ = static_cast<size_t>(n);
    }
    expect_continue_ = iequals(tmp.header("Expect"), "100-continue");
    have_headers_ = true;
  }

  size_t total;
  std::string_view body;
  if (chunked_) {
    if (!chunks_done_) {
      const Status s = decode_chunks(buf);
      if (s != Status::Done) return s;
    }
    total = chunk_end_;
    body = chunked_body_;
  } else {
    total = header_len_ + content_length_;
    if (buf.size() < total) return Status::NeedMore;
    body = buf.substr(header_len_, content_length_);
  }
  if (!parse_head(buf.substr(0, header_len_), req, false)) return Status::Error;
  req.body = body;
  consumed = total;
  // Preparar la siguiente petición (el cuerpo chunked sigue vivo hasta entonces).
  have_headers_ = false;
  scanned_ = 0;
  header_len_ = 0;
  content_length_ = 0;
  chunked_ = false;
  expect_continue_ = false;
  chunk_pos_ = 0;
  chunk_end_ = 0;
  chunks_done_ = false;
  return Status::Done;
}

}  // namespace zr::net
