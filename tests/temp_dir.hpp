// Directorio temporal único que se borra al salir del ámbito.
#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>

namespace zt {

struct TempDir {
  std::filesystem::path path;

  TempDir() {
    static std::atomic<int> counter{0};
    std::random_device rd;
    path = std::filesystem::temp_directory_path() /
           ("zenrift_test_" + std::to_string(rd()) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string str() const { return path.string(); }
};

}  // namespace zt
