#include "sketch/space_saving.hpp"

#include <algorithm>

namespace zr::sketch {

SpaceSaving::SpaceSaving(uint32_t capacity) : capacity_(capacity < 1 ? 1 : capacity) {
  counters_.resize(capacity_);
  heap_.reserve(capacity_);
  heap_pos_.resize(capacity_, 0);
}

SpaceSaving::SpaceSaving(const SpaceSaving& o) : capacity_(o.capacity_), total_(o.total_) {
  counters_.resize(capacity_);
  heap_.reserve(capacity_);
  heap_pos_.resize(capacity_, 0);
  // Reconstruir en vez de copiar: las vistas del índice deben apuntar a
  // nuestras propias cadenas.
  for (uint32_t h : o.heap_) {
    const Counter& src = o.counters_[h];
    const uint32_t slot = static_cast<uint32_t>(heap_.size());
    counters_[slot] = src;
    heap_pos_[slot] = slot;
    heap_.push_back(slot);
    index_.try_emplace(std::string_view(counters_[slot].item), slot);
  }
  // heap_ copia el orden de o.heap_, que ya es un montículo válido.
}

SpaceSaving& SpaceSaving::operator=(const SpaceSaving& o) {
  if (this != &o) {
    SpaceSaving tmp(o);
    *this = std::move(tmp);
  }
  return *this;
}

void SpaceSaving::swap_heap(size_t a, size_t b) {
  std::swap(heap_[a], heap_[b]);
  heap_pos_[heap_[a]] = static_cast<uint32_t>(a);
  heap_pos_[heap_[b]] = static_cast<uint32_t>(b);
}

void SpaceSaving::sift_down(size_t pos) {
  const size_t n = heap_.size();
  while (true) {
    const size_t l = 2 * pos + 1, r = l + 1;
    size_t smallest = pos;
    if (l < n && counters_[heap_[l]].count < counters_[heap_[smallest]].count) smallest = l;
    if (r < n && counters_[heap_[r]].count < counters_[heap_[smallest]].count) smallest = r;
    if (smallest == pos) return;
    swap_heap(pos, smallest);
    pos = smallest;
  }
}

void SpaceSaving::sift_up(size_t pos) {
  while (pos > 0) {
    const size_t parent = (pos - 1) / 2;
    if (counters_[heap_[parent]].count <= counters_[heap_[pos]].count) return;
    swap_heap(pos, parent);
    pos = parent;
  }
}

void SpaceSaving::add(std::string_view item, uint64_t count) {
  if (count == 0) return;
  total_ += count;
  if (uint32_t* slot = index_.find(item)) {
    counters_[*slot].count += count;
    sift_down(heap_pos_[*slot]);  // la cuenta crece: baja en el montículo de mínimos
    return;
  }
  if (!full()) {
    const uint32_t slot = static_cast<uint32_t>(heap_.size());
    Counter& c = counters_[slot];
    c.item.assign(item);
    c.count = count;
    c.error = 0;
    heap_pos_[slot] = slot;
    heap_.push_back(slot);
    index_.try_emplace(std::string_view(c.item), slot);
    sift_up(heap_.size() - 1);
    return;
  }
  // Reemplazar el mínimo: hereda su cuenta como error.
  const uint32_t slot = heap_[0];
  Counter& c = counters_[slot];
  index_.erase(std::string_view(c.item));
  const uint64_t floor = c.count;
  c.item.assign(item);
  c.error = floor;
  c.count = floor + count;
  index_.try_emplace(std::string_view(c.item), slot);
  sift_down(0);
}

void SpaceSaving::merge(const SpaceSaving& o) {
  if (o.heap_.empty()) return;
  const uint64_t fill_a = full() ? min_count() : 0;
  const uint64_t fill_b = o.full() ? o.min_count() : 0;
  FlatMap<std::string_view, Entry> combined(heap_.size() + o.heap_.size());
  for (uint32_t h : heap_) {
    const Counter& c = counters_[h];
    combined.try_emplace(std::string_view(c.item), Entry{c.item, c.count + fill_b, c.error + fill_b});
  }
  for (uint32_t h : o.heap_) {
    const Counter& c = o.counters_[h];
    auto [e, inserted] = combined.try_emplace(std::string_view(c.item), Entry{c.item, c.count + fill_a, c.error + fill_a});
    if (!inserted) {
      // Estaba en ambos: se suman las cuentas reales, sin relleno.
      e->count = e->count - fill_b + c.count;
      e->error = e->error - fill_b + c.error;
    }
  }
  std::vector<Entry> entries;
  entries.reserve(combined.size());
  combined.for_each([&entries](std::string_view, Entry& e) { entries.push_back(std::move(e)); });
  const uint64_t new_total = total_ + o.total_;
  rebuild(std::move(entries));
  total_ = new_total;
}

void SpaceSaving::rebuild(std::vector<Entry>&& entries) {
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.count != b.count ? a.count > b.count : a.item < b.item;
  });
  if (entries.size() > capacity_) entries.resize(capacity_);
  index_.clear();
  heap_.clear();
  for (auto& e : entries) {
    const uint32_t slot = static_cast<uint32_t>(heap_.size());
    Counter& c = counters_[slot];
    c.item = std::move(e.item);
    c.count = e.count;
    c.error = e.error;
    heap_pos_[slot] = slot;
    heap_.push_back(slot);
    index_.try_emplace(std::string_view(c.item), slot);
  }
  // Orden descendente -> convertir en montículo de mínimos.
  for (size_t i = heap_.size() / 2; i-- > 0;) sift_down(i);
}

std::vector<SpaceSaving::Entry> SpaceSaving::top(size_t k) const {
  std::vector<Entry> out;
  out.reserve(heap_.size());
  for (uint32_t h : heap_) out.push_back({counters_[h].item, counters_[h].count, counters_[h].error});
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    return a.count != b.count ? a.count > b.count : a.item < b.item;
  });
  if (out.size() > k) out.resize(k);
  return out;
}

size_t SpaceSaving::memory_bytes() const noexcept {
  size_t b = sizeof(*this) + counters_.capacity() * sizeof(Counter) + heap_.capacity() * 8 + index_.capacity() * 25;
  for (const auto& c : counters_) b += c.item.capacity() > 15 ? c.item.capacity() : 0;
  return b;
}

}  // namespace zr::sketch
