// Despertador del bucle de eventos: otro hilo llama a wake() y el poller ve
// el descriptor legible. eventfd en Linux; par de sockets loopback en el resto.
#pragma once

#include <string>

#include "net/socket.hpp"

namespace zr::net {

class Waker {
 public:
  Waker() = default;
  ~Waker();
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;

  bool open(std::string& err);
  socket_t fd() const noexcept { return read_; }
  // Seguro entre hilos. Cada llamada escribe: quien la usa decide cuándo hace
  // falta (p. ej. solo al encolar en una cola vacía).
  void wake() noexcept;
  // Lo llama el bucle de eventos al ver el descriptor legible, ANTES de vaciar
  // la cola de trabajo: así un aviso que llegue después nunca se pierde.
  void drain() noexcept;

 private:
  socket_t read_ = kInvalidSocket;
  socket_t write_ = kInvalidSocket;
  bool eventfd_ = false;
};

}  // namespace zr::net
