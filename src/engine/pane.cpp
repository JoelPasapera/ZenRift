#include "engine/pane.hpp"

#include <cstring>

#include "core/hash.hpp"

namespace zr::engine {

Pane::Pane(std::shared_ptr<const AggLayout> layout, int64_t start, int64_t end)
    : layout_(std::move(layout)), start_(start), end_(end), arena_(16 * 1024) {}

Pane::~Pane() {
  for (const auto& g : groups_) layout_->destroy_block(g.block);
}

char* Pane::group(std::string_view key, uint64_t hash, size_t max_groups) {
  if (uint32_t* idx = index_.find(KeyRef{key, hash})) return groups_[*idx].block;
  if (groups_.size() >= max_groups) return nullptr;
  const std::string_view stored = arena_.copy(key);
  char* block = static_cast<char*>(arena_.alloc(layout_->block_size(), layout_->block_align()));
  layout_->init_block(block);
  groups_.push_back(Group{stored, hash, block});
  index_.try_emplace(KeyRef{stored, hash}, static_cast<uint32_t>(groups_.size() - 1));
  return block;
}

void Pane::merge_from(const Pane& other, size_t max_groups) {
  for (const auto& g : other.groups_) {
    if (uint32_t* idx = index_.find(KeyRef{g.key, g.hash})) {
      layout_->merge_block(groups_[*idx].block, g.block);
      continue;
    }
    if (groups_.size() >= max_groups) {
      ++dropped_groups;
      continue;
    }
    const std::string_view stored = arena_.copy(g.key);
    char* block = static_cast<char*>(arena_.alloc(layout_->block_size(), layout_->block_align()));
    layout_->copy_block(block, g.block);
    groups_.push_back(Group{stored, g.hash, block});
    index_.try_emplace(KeyRef{stored, g.hash}, static_cast<uint32_t>(groups_.size() - 1));
  }
  events += other.events;
  dropped_groups += other.dropped_groups;
}

std::unique_ptr<Pane> Pane::clone() const {
  auto p = std::make_unique<Pane>(layout_, start_, end_);
  p->index_.reserve(groups_.size());
  p->groups_.reserve(groups_.size());
  for (const auto& g : groups_) {
    const std::string_view stored = p->arena_.copy(g.key);
    char* block = static_cast<char*>(p->arena_.alloc(layout_->block_size(), layout_->block_align()));
    layout_->copy_block(block, g.block);
    p->groups_.push_back(Group{stored, g.hash, block});
    p->index_.try_emplace(KeyRef{stored, g.hash}, static_cast<uint32_t>(p->groups_.size() - 1));
  }
  p->events = events;
  p->dropped_groups = dropped_groups;
  return p;
}

}  // namespace zr::engine
