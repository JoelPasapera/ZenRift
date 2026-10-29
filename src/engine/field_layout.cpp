#include "engine/field_layout.hpp"

#include "core/hash.hpp"

namespace zr::engine {

void FieldLayout::build(std::vector<std::string> paths) {
  paths_ = std::move(paths);
  leaves_ = FlatMap<uint64_t, int32_t>(paths_.size());
  prefixes_ = FlatMap<uint64_t, uint8_t>();
  for (size_t i = 0; i < paths_.size(); ++i) {
    const std::string& p = paths_[i];
    leaves_.try_emplace(hash_str(p), static_cast<int32_t>(i));
    for (size_t dot = p.find('.'); dot != std::string::npos; dot = p.find('.', dot + 1))
      prefixes_.try_emplace(hash_str(std::string_view(p).substr(0, dot)), uint8_t{1});
  }
}

int32_t FieldLayout::slot_of(std::string_view path) const noexcept {
  return find_leaf(hash_str(path), path);
}

int32_t FieldLayout::find_leaf(uint64_t hash, std::string_view path) const noexcept {
  const int32_t* slot = leaves_.find(hash);
  if (!slot || paths_[static_cast<size_t>(*slot)] != path) return -1;
  return *slot;
}

}  // namespace zr::engine
