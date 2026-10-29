// Archivo binario de bajo nivel, portable (POSIX y Windows): escritura al
// final, lectura posicional, fsync y truncado. Es lo único del almacenamiento
// que toca el sistema operativo.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace zr::storage {

class File {
 public:
  File() = default;
  ~File();
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& o) noexcept;
  File& operator=(File&& o) noexcept;

  // Abre para lectura/escritura, creándolo si no existe. Las escrituras van al final.
  bool open(const std::string& path, std::string& err);
  bool is_open() const noexcept { return fd_ >= 0; }
  void close() noexcept;

  bool append(const void* data, size_t n, std::string& err);
  // Lee hasta n bytes en `pos`; got = bytes leídos (0 al final del archivo).
  bool read_at(void* buf, size_t n, uint64_t pos, size_t& got, std::string& err);
  bool sync(std::string& err);
  bool truncate(uint64_t size, std::string& err);
  bool size(uint64_t& out, std::string& err) const;

 private:
  int fd_ = -1;
  std::string path_;
};

std::string errno_text(int code);

}  // namespace zr::storage
