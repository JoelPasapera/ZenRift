#include "sketch/hyperloglog.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace zr::sketch {

namespace {

// σ(x) = x + Σ_{k≥1} x^(2^k) 2^(k-1)
double sigma(double x) {
  if (x == 1.0) return std::numeric_limits<double>::infinity();
  double y = 1.0, z = x, prev;
  do {
    x *= x;
    prev = z;
    z += x * y;
    y += y;
  } while (z != prev);
  return z;
}

// τ(x) = (1/3) (1 - x - Σ_{k≥1} (1 - x^(2^-k))² 2^-k)
double tau(double x) {
  if (x == 0.0 || x == 1.0) return 0.0;
  double y = 1.0, z = 1.0 - x, prev;
  do {
    x = std::sqrt(x);
    prev = z;
    y *= 0.5;
    z -= (1.0 - x) * (1.0 - x) * y;
  } while (z != prev);
  return z / 3.0;
}

double estimate_from_histogram(const uint32_t (&c)[HyperLogLog::kQ + 2]) {
  constexpr double m = HyperLogLog::kM;
  constexpr int q = HyperLogLog::kQ;
  double z = m * tau(1.0 - c[q + 1] / m);
  for (int k = q; k >= 1; --k) z = 0.5 * (z + c[k]);
  z += m * sigma(c[0] / m);
  constexpr double alpha_inf = 0.721347520444481703680;  // 1 / (2 ln 2)
  return alpha_inf * m * m / z;
}

}  // namespace

HyperLogLog::HyperLogLog(const HyperLogLog& o) : sparse_(o.sparse_) {
  if (o.dense_) {
    dense_ = std::make_unique<uint8_t[]>(kM);
    std::memcpy(dense_.get(), o.dense_.get(), kM);
  }
}

HyperLogLog& HyperLogLog::operator=(const HyperLogLog& o) {
  if (this != &o) {
    HyperLogLog tmp(o);
    *this = std::move(tmp);
  }
  return *this;
}

void HyperLogLog::add_hash(uint64_t h) {
  const uint32_t idx = static_cast<uint32_t>(h >> kQ);
  const uint64_t w = h << kP;  // los kQ bits restantes, alineados arriba
  const int lz = w == 0 ? kQ : std::countl_zero(w);
  const uint8_t rho = static_cast<uint8_t>(lz >= kQ ? kQ + 1 : lz + 1);
  set_register(idx, rho);
}

void HyperLogLog::set_register(uint32_t idx, uint8_t value) {
  if (dense_) {
    if (dense_[idx] < value) dense_[idx] = value;
    return;
  }
  auto [slot, inserted] = sparse_.try_emplace(idx, value);
  if (!inserted && *slot < value) *slot = value;
  if (inserted && sparse_.size() > kSparseMax) to_dense();
}

void HyperLogLog::to_dense() {
  dense_ = std::make_unique<uint8_t[]>(kM);
  std::memset(dense_.get(), 0, kM);
  sparse_.for_each([this](uint32_t idx, uint8_t v) { dense_[idx] = v; });
  sparse_ = FlatMap<uint32_t, uint8_t>();
}

void HyperLogLog::merge(const HyperLogLog& o) {
  if (o.dense_) {
    if (!dense_) to_dense();
    for (uint32_t i = 0; i < kM; ++i)
      if (dense_[i] < o.dense_[i]) dense_[i] = o.dense_[i];
    return;
  }
  o.sparse_.for_each([this](uint32_t idx, uint8_t v) { set_register(idx, v); });
}

double HyperLogLog::estimate() const {
  uint32_t c[kQ + 2] = {};
  if (dense_) {
    for (uint32_t i = 0; i < kM; ++i) ++c[dense_[i]];
  } else {
    c[0] = kM - static_cast<uint32_t>(sparse_.size());
    sparse_.for_each([&c](uint32_t, uint8_t v) { ++c[v]; });
  }
  if (c[0] == kM) return 0.0;
  return estimate_from_histogram(c);
}

size_t HyperLogLog::memory_bytes() const noexcept {
  return sizeof(*this) + (dense_ ? kM : sparse_.capacity() * (sizeof(uint32_t) * 2 + 1));
}

}  // namespace zr::sketch
