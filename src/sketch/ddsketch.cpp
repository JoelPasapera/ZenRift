#include "sketch/ddsketch.hpp"

#include <algorithm>
#include <cmath>

namespace zr::sketch {

namespace {

inline void add_saturating(uint32_t& slot, uint64_t n) noexcept {
  const uint64_t v = static_cast<uint64_t>(slot) + n;
  slot = v > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(v);
}

}  // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void DDSketch::Store::add(int32_t key, uint64_t n, uint32_t max_bins) {
  if (n == 0) return;
  if (sparse_) {
    auto it = std::lower_bound(sparse_bins_.begin(), sparse_bins_.end(), key,
                               [](const std::pair<int32_t, uint32_t>& b, int32_t k) { return b.first < k; });
    if (it != sparse_bins_.end() && it->first == key) {
      add_saturating(it->second, n);
      total_ += n;
      return;
    }
    if (sparse_bins_.size() < kSparseMax) {
      uint32_t c = 0;
      add_saturating(c, n);
      sparse_bins_.insert(it, {key, c});
      total_ += n;
      return;
    }
    to_dense(max_bins);
  }
  dense_add(key, n, max_bins);
}

void DDSketch::Store::to_dense(uint32_t max_bins) {
  sparse_ = false;
  std::vector<std::pair<int32_t, uint32_t>> old;
  old.swap(sparse_bins_);
  const uint64_t saved_total = total_;
  total_ = 0;
  bins_.clear();
  for (const auto& [k, c] : old) dense_add(k, c, max_bins);
  total_ = saved_total;
}

void DDSketch::Store::dense_add(int32_t key, uint64_t n, uint32_t max_bins) {
  if (bins_.empty()) {
    offset_ = key;
    bins_.assign(1, 0);
  }
  const int64_t lo = offset_;
  const int64_t hi = offset_ + static_cast<int64_t>(bins_.size()) - 1;
  if (key < lo) {
    // Extender hacia abajo hasta donde permita el tope; lo que quede por
    // debajo se colapsa en la cubeta más baja.
    const int64_t lowest_allowed = hi - static_cast<int64_t>(max_bins) + 1;
    const int64_t new_lo = std::max<int64_t>(key, lowest_allowed);
    if (new_lo < lo) {
      bins_.insert(bins_.begin(), static_cast<size_t>(lo - new_lo), 0u);
      offset_ = static_cast<int32_t>(new_lo);
    }
    if (key < offset_) key = offset_;
  } else if (key > hi) {
    const int64_t span = static_cast<int64_t>(key) - lo + 1;
    if (span <= static_cast<int64_t>(max_bins)) {
      bins_.resize(static_cast<size_t>(span), 0u);
    } else {
      // Desplazar la ventana hacia arriba colapsando las cubetas más bajas.
      const int64_t new_lo = static_cast<int64_t>(key) - max_bins + 1;
      uint64_t collapsed = 0;
      std::vector<uint32_t> fresh(max_bins, 0u);
      for (size_t i = 0; i < bins_.size(); ++i) {
        const int64_t k = lo + static_cast<int64_t>(i);
        if (k < new_lo)
          collapsed += bins_[i];
        else
          fresh[static_cast<size_t>(k - new_lo)] = bins_[i];
      }
      add_saturating(fresh[0], collapsed);
      bins_.swap(fresh);
      offset_ = static_cast<int32_t>(new_lo);
    }
  }
  add_saturating(bins_[static_cast<size_t>(key - offset_)], n);
  total_ += n;
}

void DDSketch::Store::merge(const Store& o, uint32_t max_bins) {
  if (o.total_ == 0) return;
  if (o.sparse_) {
    for (const auto& [k, c] : o.sparse_bins_) add(k, c, max_bins);
    return;
  }
  if (sparse_) to_dense(max_bins);
  for (size_t i = 0; i < o.bins_.size(); ++i)
    if (o.bins_[i] != 0) dense_add(o.offset_ + static_cast<int32_t>(i), o.bins_[i], max_bins);
}

int32_t DDSketch::Store::key_at_rank(double rank) const noexcept {
  double acc = 0;
  if (sparse_) {
    for (const auto& [k, c] : sparse_bins_) {
      acc += c;
      if (acc > rank) return k;
    }
    return sparse_bins_.empty() ? 0 : sparse_bins_.back().first;
  }
  for (size_t i = 0; i < bins_.size(); ++i) {
    acc += bins_[i];
    if (acc > rank) return offset_ + static_cast<int32_t>(i);
  }
  return offset_ + static_cast<int32_t>(bins_.size()) - 1;
}

size_t DDSketch::Store::memory_bytes() const noexcept {
  return sparse_bins_.capacity() * sizeof(std::pair<int32_t, uint32_t>) + bins_.capacity() * sizeof(uint32_t);
}

// ---------------------------------------------------------------------------
// DDSketch
// ---------------------------------------------------------------------------

DDSketch::DDSketch(double relative_accuracy, uint32_t max_bins)
    : alpha_(relative_accuracy), max_bins_(max_bins) {
  gamma_ = (1 + alpha_) / (1 - alpha_);
  ln_gamma_ = std::log(gamma_);
  multiplier_ = 1.0 / ln_gamma_;
  // Valores más pequeños que esto (en magnitud) cuentan como cero. Con
  // α >= 0.1 % la clave más baja posible cabe holgadamente en un int32.
  min_indexable_ = std::numeric_limits<double>::min() * gamma_;
}

int32_t DDSketch::key(double v) const noexcept {
  return static_cast<int32_t>(std::ceil(std::log(v) * multiplier_));
}

double DDSketch::value(int32_t k) const noexcept {
  return 2.0 * std::exp(static_cast<double>(k) * ln_gamma_) / (gamma_ + 1.0);
}

void DDSketch::add(double v) {
  if (!std::isfinite(v)) return;
  if (v > min_indexable_)
    pos_.add(key(v), 1, max_bins_);
  else if (v < -min_indexable_)
    neg_.add(key(-v), 1, max_bins_);
  else
    ++zero_count_;
  ++count_;
  sum_ += v;
  if (v < min_) min_ = v;
  if (v > max_) max_ = v;
}

void DDSketch::merge(const DDSketch& o) {
  if (o.count_ == 0) return;
  pos_.merge(o.pos_, max_bins_);
  neg_.merge(o.neg_, max_bins_);
  zero_count_ += o.zero_count_;
  count_ += o.count_;
  sum_ += o.sum_;
  min_ = std::min(min_, o.min_);
  max_ = std::max(max_, o.max_);
}

double DDSketch::quantile(double q) const {
  if (count_ == 0 || !(q >= 0 && q <= 1)) return std::numeric_limits<double>::quiet_NaN();
  if (q == 0) return min_;
  if (q == 1) return max_;
  const double rank = q * static_cast<double>(count_ - 1);
  const double neg_total = static_cast<double>(neg_.total());
  double v;
  if (rank < neg_total) {
    v = -value(neg_.key_at_rank(neg_total - 1 - rank));
  } else if (rank < neg_total + static_cast<double>(zero_count_)) {
    v = 0;
  } else {
    v = value(pos_.key_at_rank(rank - neg_total - static_cast<double>(zero_count_)));
  }
  return std::clamp(v, min_, max_);
}

size_t DDSketch::memory_bytes() const noexcept { return sizeof(*this) + pos_.memory_bytes() + neg_.memory_bytes(); }

}  // namespace zr::sketch
