#include "net/waker.hpp"

#ifdef __linux__
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace zr::net {

Waker::~Waker() {
  if (eventfd_) {
#ifdef __linux__
    ::close(read_);
#endif
  } else {
    close_socket(read_);
    close_socket(write_);
  }
}

bool Waker::open(std::string& err) {
#ifdef __linux__
  const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd >= 0) {
    read_ = write_ = fd;
    eventfd_ = true;
    return true;
  }
#endif
  return socket_pair(read_, write_, err);
}

void Waker::wake() noexcept {
#ifdef __linux__
  if (eventfd_) {
    const uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(write_, &one, sizeof one);
    return;
  }
#endif
  const char b = 1;
  send_some(write_, &b, 1);
}

void Waker::drain() noexcept {
#ifdef __linux__
  if (eventfd_) {
    uint64_t v;
    [[maybe_unused]] const ssize_t n = ::read(read_, &v, sizeof v);
    return;
  }
#endif
  char buf[256];
  while (recv_some(read_, buf, sizeof buf) > 0) {
  }
}

}  // namespace zr::net
