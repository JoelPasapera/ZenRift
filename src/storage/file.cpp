#include "storage/file.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#include "core/strings.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zr::storage {

std::string errno_text(int code) {
#ifdef _WIN32
  char buf[256];
  strerror_s(buf, sizeof buf, code);
  return buf;
#else
  return std::strerror(code);
#endif
}

File::~File() { close(); }

File::File(File&& o) noexcept : fd_(o.fd_), path_(std::move(o.path_)) { o.fd_ = -1; }

File& File::operator=(File&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    path_ = std::move(o.path_);
    o.fd_ = -1;
  }
  return *this;
}

bool File::open(const std::string& path, std::string& err) {
  close();
#ifdef _WIN32
  fd_ = ::_open(path.c_str(), _O_RDWR | _O_CREAT | _O_APPEND | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
#endif
  if (fd_ < 0) {
    err = cat("no se pudo abrir ", path, ": ", errno_text(errno));
    return false;
  }
  path_ = path;
  return true;
}

void File::close() noexcept {
  if (fd_ >= 0) {
#ifdef _WIN32
    ::_close(fd_);
#else
    ::close(fd_);
#endif
    fd_ = -1;
  }
}

bool File::append(const void* data, size_t n, std::string& err) {
  const char* p = static_cast<const char*>(data);
  while (n > 0) {
#ifdef _WIN32
    const unsigned chunk = n > (1u << 30) ? (1u << 30) : static_cast<unsigned>(n);
    const int w = ::_write(fd_, p, chunk);
#else
    const ssize_t w = ::write(fd_, p, n);
#endif
    if (w < 0) {
      if (errno == EINTR) continue;
      err = cat("error al escribir ", path_, ": ", errno_text(errno));
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool File::read_at(void* buf, size_t n, uint64_t pos, size_t& got, std::string& err) {
  got = 0;
  char* p = static_cast<char*>(buf);
#ifdef _WIN32
  // La CRT de Windows no tiene pread: posicionar y leer. Cada partición solo
  // la usa su hilo dueño, así que no hay carreras con el puntero del archivo.
  if (::_lseeki64(fd_, static_cast<__int64>(pos), SEEK_SET) < 0) {
    err = cat("error al posicionar en ", path_, ": ", errno_text(errno));
    return false;
  }
  while (got < n) {
    const unsigned chunk = (n - got) > (1u << 30) ? (1u << 30) : static_cast<unsigned>(n - got);
    const int r = ::_read(fd_, p + got, chunk);
    if (r < 0) {
      err = cat("error al leer ", path_, ": ", errno_text(errno));
      return false;
    }
    if (r == 0) break;
    got += static_cast<size_t>(r);
  }
#else
  while (got < n) {
    const ssize_t r = ::pread(fd_, p + got, n - got, static_cast<off_t>(pos + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      err = cat("error al leer ", path_, ": ", errno_text(errno));
      return false;
    }
    if (r == 0) break;
    got += static_cast<size_t>(r);
  }
#endif
  return true;
}

bool File::sync(std::string& err) {
#ifdef _WIN32
  if (::_commit(fd_) != 0) {
#elif defined(__APPLE__)
  if (::fsync(fd_) != 0) {
#else
  if (::fdatasync(fd_) != 0) {
#endif
    err = cat("error en fsync de ", path_, ": ", errno_text(errno));
    return false;
  }
  return true;
}

bool File::truncate(uint64_t size, std::string& err) {
#ifdef _WIN32
  if (::_chsize_s(fd_, static_cast<__int64>(size)) != 0) {
#else
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
#endif
    err = cat("error al truncar ", path_, ": ", errno_text(errno));
    return false;
  }
  return true;
}

bool File::size(uint64_t& out, std::string& err) const {
#ifdef _WIN32
  struct _stat64 st;
  if (::_fstat64(fd_, &st) != 0) {
#else
  struct stat st;
  if (::fstat(fd_, &st) != 0) {
#endif
    err = cat("error en fstat de ", path_, ": ", errno_text(errno));
    return false;
  }
  out = static_cast<uint64_t>(st.st_size);
  return true;
}

}  // namespace zr::storage
