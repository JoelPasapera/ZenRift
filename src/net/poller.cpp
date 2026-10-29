#include "net/poller.hpp"

#include <unordered_map>

#include "core/strings.hpp"

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <poll.h>
#endif

namespace zr::net {

namespace {

#ifdef __linux__
// ---------------------------------------------------------------- epoll
class EpollPoller final : public Poller {
 public:
  bool init(std::string& err) {
    fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd_ < 0) {
      err = cat("epoll_create1: ", error_text(last_error()));
      return false;
    }
    return true;
  }
  ~EpollPoller() override {
    if (fd_ >= 0) ::close(fd_);
  }

  bool add(socket_t s, uint64_t token, bool want_write) override { return ctl(EPOLL_CTL_ADD, s, token, want_write); }
  bool modify(socket_t s, uint64_t token, bool want_write) override {
    return ctl(EPOLL_CTL_MOD, s, token, want_write);
  }
  void remove(socket_t s) override { ::epoll_ctl(fd_, EPOLL_CTL_DEL, s, nullptr); }

  int wait(std::vector<PollEvent>& out, int timeout_ms) override {
    epoll_event evs[256];
    const int n = ::epoll_wait(fd_, evs, 256, timeout_ms);
    out.clear();
    for (int i = 0; i < n; ++i) {
      PollEvent e;
      e.token = evs[i].data.u64;
      e.readable = evs[i].events & (EPOLLIN | EPOLLRDHUP);
      e.writable = evs[i].events & EPOLLOUT;
      e.hangup = evs[i].events & (EPOLLHUP | EPOLLERR);
      out.push_back(e);
    }
    return n < 0 ? 0 : n;
  }

  const char* name() const noexcept override { return "epoll"; }

 private:
  bool ctl(int op, socket_t s, uint64_t token, bool want_write) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (want_write ? EPOLLOUT : 0u);
    ev.data.u64 = token;
    return ::epoll_ctl(fd_, op, s, &ev) == 0;
  }
  int fd_ = -1;
};
#endif

// ---------------------------------------------------------------- poll / WSAPoll
class PollPoller final : public Poller {
 public:
  bool add(socket_t s, uint64_t token, bool want_write) override {
    if (index_.count(s)) return false;
    index_[s] = fds_.size();
    pollfd p{};
    p.fd = s;
    p.events = static_cast<short>(POLLIN | (want_write ? POLLOUT : 0));
    fds_.push_back(p);
    tokens_.push_back(token);
    return true;
  }

  bool modify(socket_t s, uint64_t token, bool want_write) override {
    auto it = index_.find(s);
    if (it == index_.end()) return false;
    fds_[it->second].events = static_cast<short>(POLLIN | (want_write ? POLLOUT : 0));
    tokens_[it->second] = token;
    return true;
  }

  void remove(socket_t s) override {
    auto it = index_.find(s);
    if (it == index_.end()) return;
    const size_t i = it->second, last = fds_.size() - 1;
    if (i != last) {
      fds_[i] = fds_[last];
      tokens_[i] = tokens_[last];
      index_[fds_[i].fd] = i;
    }
    fds_.pop_back();
    tokens_.pop_back();
    index_.erase(it);
  }

  int wait(std::vector<PollEvent>& out, int timeout_ms) override {
    out.clear();
#ifdef _WIN32
    const int n = ::WSAPoll(fds_.data(), static_cast<ULONG>(fds_.size()), timeout_ms);
#else
    const int n = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()), timeout_ms);
#endif
    if (n <= 0) return 0;
    for (size_t i = 0; i < fds_.size(); ++i) {
      const short re = fds_[i].revents;
      if (!re) continue;
      PollEvent e;
      e.token = tokens_[i];
      e.readable = re & POLLIN;
      e.writable = re & POLLOUT;
      e.hangup = re & (POLLHUP | POLLERR | POLLNVAL);
      out.push_back(e);
    }
    return static_cast<int>(out.size());
  }

  const char* name() const noexcept override {
#ifdef _WIN32
    return "WSAPoll";
#else
    return "poll";
#endif
  }

 private:
#ifdef _WIN32
  using pollfd = WSAPOLLFD;
#endif
  std::vector<pollfd> fds_;
  std::vector<uint64_t> tokens_;
  std::unordered_map<socket_t, size_t> index_;
};

}  // namespace

std::unique_ptr<Poller> Poller::create(std::string_view kind, std::string& err) {
#ifdef __linux__
  if (kind.empty() || kind == "epoll") {
    auto p = std::make_unique<EpollPoller>();
    if (p->init(err)) return p;
    if (kind == "epoll") return nullptr;
  }
#endif
  if (!kind.empty() && kind != "poll" && kind != "epoll") {
    err = cat("multiplexor desconocido '", kind, "' (usa epoll o poll)");
    return nullptr;
  }
#ifndef __linux__
  if (kind == "epoll") {
    err = "epoll solo existe en Linux";
    return nullptr;
  }
#endif
  return std::make_unique<PollPoller>();
}

}  // namespace zr::net
