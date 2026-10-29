// Multiplexor de E/S: epoll en Linux, poll()/WSAPoll en el resto (macOS,
// BSD, Windows). Notifica legibilidad/escritura por socket con un token.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "net/socket.hpp"

namespace zr::net {

struct PollEvent {
  uint64_t token = 0;
  bool readable = false;
  bool writable = false;
  bool hangup = false;
};

class Poller {
 public:
  virtual ~Poller() = default;
  virtual bool add(socket_t s, uint64_t token, bool want_write) = 0;
  virtual bool modify(socket_t s, uint64_t token, bool want_write) = 0;
  virtual void remove(socket_t s) = 0;
  // Espera hasta timeout_ms (-1: indefinido). Devuelve el número de eventos.
  virtual int wait(std::vector<PollEvent>& out, int timeout_ms) = 0;
  virtual const char* name() const noexcept = 0;

  // kind: "epoll", "poll" o vacío (el mejor disponible).
  static std::unique_ptr<Poller> create(std::string_view kind, std::string& err);
};

}  // namespace zr::net
