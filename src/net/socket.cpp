#include "net/socket.hpp"

#include <cstring>

#include "core/strings.hpp"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace zr::net {

bool net_init(std::string& err) {
#ifdef _WIN32
  WSADATA data;
  const int rc = WSAStartup(MAKEWORD(2, 2), &data);
  if (rc != 0) {
    err = cat("WSAStartup falló: ", rc);
    return false;
  }
#else
  std::signal(SIGPIPE, SIG_IGN);  // escribir en un socket cerrado no debe matar el proceso
  (void)err;
#endif
  return true;
}

int last_error() noexcept {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool would_block(int err) noexcept {
#ifdef _WIN32
  return err == WSAEWOULDBLOCK;
#else
  return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

std::string error_text(int err) {
#ifdef _WIN32
  char buf[256] = {};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(err), 0, buf,
                 sizeof buf, nullptr);
  return cat(buf, " (", err, ")");
#else
  return cat(std::strerror(err), " (", err, ")");
#endif
}

void close_socket(socket_t s) noexcept {
  if (s == kInvalidSocket) return;
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

bool set_nonblocking(socket_t s) noexcept {
#ifdef _WIN32
  u_long on = 1;
  return ::ioctlsocket(s, FIONBIO, &on) == 0;
#else
  const int flags = ::fcntl(s, F_GETFL, 0);
  return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void set_nodelay(socket_t s) noexcept {
  int on = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

namespace {

bool resolve(const std::string& host, uint16_t port, bool passive, addrinfo*& out, std::string& err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &out);
  if (rc != 0) {
#ifdef _WIN32
    err = cat("no se pudo resolver '", host, "': ", rc);
#else
    err = cat("no se pudo resolver '", host, "': ", gai_strerror(rc));
#endif
    return false;
  }
  return true;
}

}  // namespace

socket_t listen_tcp(const std::string& host, uint16_t port, std::string& err) {
  addrinfo* res = nullptr;
  if (!resolve(host, port, true, res, err)) return kInvalidSocket;
  socket_t s = kInvalidSocket;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == kInvalidSocket) continue;
    int on = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
    if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && ::listen(s, 512) == 0 && set_nonblocking(s))
      break;
    err = cat("no se pudo escuchar en ", host, ":", port, ": ", error_text(last_error()));
    close_socket(s);
    s = kInvalidSocket;
  }
  ::freeaddrinfo(res);
  if (s != kInvalidSocket) err.clear();
  return s;
}

socket_t connect_tcp(const std::string& host, uint16_t port, std::string& err) {
  addrinfo* res = nullptr;
  if (!resolve(host, port, false, res, err)) return kInvalidSocket;
  socket_t s = kInvalidSocket;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == kInvalidSocket) continue;
    if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
    err = cat("no se pudo conectar a ", host, ":", port, ": ", error_text(last_error()));
    close_socket(s);
    s = kInvalidSocket;
  }
  ::freeaddrinfo(res);
  if (s != kInvalidSocket) {
    set_nodelay(s);
    err.clear();
  }
  return s;
}

socket_t accept_conn(socket_t listener, std::string* peer) noexcept {
  sockaddr_storage addr{};
#ifdef _WIN32
  int len = sizeof addr;
#else
  socklen_t len = sizeof addr;
#endif
  const socket_t s = ::accept(listener, reinterpret_cast<sockaddr*>(&addr), &len);
  if (s == kInvalidSocket) return s;
  if (peer) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET)
      ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr, buf, sizeof buf);
    else if (addr.ss_family == AF_INET6)
      ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr, buf, sizeof buf);
    *peer = buf;
  }
  return s;
}

long send_some(socket_t s, const char* data, size_t n) noexcept {
#ifdef _WIN32
  return ::send(s, data, static_cast<int>(n > (1u << 30) ? (1u << 30) : n), 0);
#elif defined(MSG_NOSIGNAL)
  return static_cast<long>(::send(s, data, n, MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(s, data, n, 0));
#endif
}

long recv_some(socket_t s, char* buf, size_t n) noexcept {
#ifdef _WIN32
  return ::recv(s, buf, static_cast<int>(n > (1u << 30) ? (1u << 30) : n), 0);
#else
  return static_cast<long>(::recv(s, buf, n, 0));
#endif
}

uint16_t local_port(socket_t s) noexcept {
  sockaddr_storage addr{};
#ifdef _WIN32
  int len = sizeof addr;
#else
  socklen_t len = sizeof addr;
#endif
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
  if (addr.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
  if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
  return 0;
}

bool socket_pair(socket_t& a, socket_t& b, std::string& err) {
  // Loopback TCP: portable a Windows, donde no hay pipes que sirvan con WSAPoll.
  socket_t l = listen_tcp("127.0.0.1", 0, err);
  if (l == kInvalidSocket) return false;
  sockaddr_in addr{};
#ifdef _WIN32
  int len = sizeof addr;
#else
  socklen_t len = sizeof addr;
#endif
  ::getsockname(l, reinterpret_cast<sockaddr*>(&addr), &len);
  a = ::socket(AF_INET, SOCK_STREAM, 0);
  if (a == kInvalidSocket || ::connect(a, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(len)) != 0) {
    err = cat("socket_pair: ", error_text(last_error()));
    close_socket(a);
    close_socket(l);
    return false;
  }
  // El listener es no bloqueante: esperar a que la conexión esté lista.
  b = kInvalidSocket;
  for (int i = 0; i < 1000 && b == kInvalidSocket; ++i) b = accept_conn(l);
  close_socket(l);
  if (b == kInvalidSocket) {
    err = "socket_pair: accept falló";
    close_socket(a);
    return false;
  }
  set_nonblocking(a);
  set_nonblocking(b);
  set_nodelay(a);
  return true;
}

}  // namespace zr::net
