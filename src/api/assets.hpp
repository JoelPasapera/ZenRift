// Archivos del panel web incrustados en el binario (ver cmake/embed.cmake).
#pragma once

#include <cstddef>
#include <string_view>

namespace zr::assets {

struct Asset {
  const char* path;          // "/index.html", "/js/main.js"...
  const char* content_type;
  const unsigned char* data;
  size_t size;
};

extern const Asset kAssets[];
extern const size_t kAssetCount;

inline const Asset* find(std::string_view path) noexcept {
  for (size_t i = 0; i < kAssetCount; ++i)
    if (path == kAssets[i].path) return &kAssets[i];
  return nullptr;
}

}  // namespace zr::assets
