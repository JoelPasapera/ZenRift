// Capa mínima de sockets portable (POSIX y Winsock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace zr::net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// Inicializa la pila de red (WSAStartup en Windows; en POSIX ignora SIGPIPE).
bool net_init(std::string& err);

int last_error() noexcept;
bool would_block(int err) noexcept;
std::string error_text(int err);

void close_socket(socket_t s) noexcept;
bool set_nonblocking(socket_t s) noexcept;
void set_nodelay(socket_t s) noexcept;

// Socket TCP escuchando en host:port (host "0.0.0.0", "127.0.0.1", "::"...).
socket_t listen_tcp(const std::string& host, uint16_t port, std::string& err);
// Conexión TCP bloqueante (para el cliente de pruebas de carga).
socket_t connect_tcp(const std::string& host, uint16_t port, std::string& err);
// Acepta una conexión (no bloqueante). kInvalidSocket si no hay ninguna.
socket_t accept_conn(socket_t listener, std::string* peer = nullptr) noexcept;

// Envío/recepción: devuelven bytes, 0 en cierre ordenado (recv) y -1 en error
// (consultar last_error(); would_block() si no hay datos o espacio).
long send_some(socket_t s, const char* data, size_t n) noexcept;
long recv_some(socket_t s, char* buf, size_t n) noexcept;

// Puerto local al que quedó ligado el socket (útil con port = 0).
uint16_t local_port(socket_t s) noexcept;

// Par de sockets conectados por loopback (para despertar al bucle de eventos
// en plataformas sin eventfd).
bool socket_pair(socket_t& a, socket_t& b, std::string& err);

}  // namespace zr::net
