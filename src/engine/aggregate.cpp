#include "engine/aggregate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <string>

#include "core/hash.hpp"
#include "json/writer.hpp"
#include "sketch/ddsketch.hpp"
#include "sketch/hyperloglog.hpp"
#include "sketch/moments.hpp"
#include "sketch/space_saving.hpp"

namespace zr::engine {

using zql::AggKind;
using zql::AggSpec;
using zql::Value;

uint32_t topk_capacity(uint32_t k) noexcept { return std::clamp<uint32_t>(k * 8, 64, 1024); }

namespace {

// ---------------------------------------------------------------- utilidades

// Suma compensada de Neumaier: evita perder precisión al sumar millones de
// valores pequeños a un total grande.
struct KSum {
  double sum = 0;
  double comp = 0;
  uint64_t n = 0;

  void add(double x) noexcept {
    const double t = sum + x;
    if (std::fabs(sum) >= std::fabs(x))
      comp += (sum - t) + x;
    else
      comp += (x - t) + sum;
    sum = t;
    ++n;
  }
  void merge(const KSum& o) noexcept {
    const uint64_t n0 = n;
    add(o.sum);
    comp += o.comp;
    n = n0 + o.n;
  }
  double value() const noexcept { return sum + comp; }
};

thread_local std::string t_enc;

const std::string& encode(const Value& v) {
  t_enc.clear();
  zql::encode_value(v, t_enc);
  return t_enc;
}

template <class T>
T& as(void* p) {
  return *static_cast<T*>(p);
}
template <class T>
const T& as(const void* p) {
  return *static_cast<const T*>(p);
}

// Operaciones genéricas para estados trivialmente copiables.
template <class T>
void pod_init(void* st) {
  ::new (st) T();
}
void pod_destroy(void*) {}
template <class T>
void pod_copy(void* dst, const void* src) {
  std::memcpy(dst, src, sizeof(T));
}

Value number_or_null(double v) { return std::isfinite(v) ? Value::number(v) : Value::null(); }

// ---------------------------------------------------------------- count

void count_update(void* st, const Value&, int64_t, const AggSpec&) { ++as<uint64_t>(st); }
void count_values_update(void* st, const Value& v, int64_t, const AggSpec&) {
  if (!v.is_null()) ++as<uint64_t>(st);
}
void count_merge(void* d, const void* s, const AggSpec&) { as<uint64_t>(d) += as<uint64_t>(s); }
Value count_result(const void* st, const AggSpec&, const ResultContext&) {
  return Value::number(static_cast<double>(as<uint64_t>(st)));
}

// ---------------------------------------------------------------- sum / rate

void sum_update(void* st, const Value& v, int64_t, const AggSpec&) {
  double x;
  if (zql::to_number(v, x) && std::isfinite(x)) as<KSum>(st).add(x);
}
void sum_merge(void* d, const void* s, const AggSpec&) { as<KSum>(d).merge(as<KSum>(s)); }
Value sum_result(const void* st, const AggSpec&, const ResultContext&) {
  const KSum& k = as<KSum>(st);
  return k.n ? number_or_null(k.value()) : Value::null();
}

void rate_update(void* st, const Value& v, int64_t, const AggSpec& spec) {
  if (!spec.has_arg) {
    as<KSum>(st).add(1.0);
    return;
  }
  double x;
  if (zql::to_number(v, x) && std::isfinite(x)) as<KSum>(st).add(x);
}
Value rate_result(const void* st, const AggSpec&, const ResultContext& ctx) {
  if (ctx.window_ms <= 0) return Value::null();
  return number_or_null(as<KSum>(st).value() / (static_cast<double>(ctx.window_ms) / 1000.0));
}

// ---------------------------------------------------------------- momentos

void moments_update(void* st, const Value& v, int64_t, const AggSpec&) {
  double x;
  if (zql::to_number(v, x) && std::isfinite(x)) as<sketch::Moments>(st).add(x);
}
void moments_merge(void* d, const void* s, const AggSpec&) { as<sketch::Moments>(d).merge(as<sketch::Moments>(s)); }
Value avg_result(const void* st, const AggSpec&, const ResultContext&) {
  const auto& m = as<sketch::Moments>(st);
  return m.n ? number_or_null(m.mean) : Value::null();
}
Value stddev_result(const void* st, const AggSpec&, const ResultContext&) {
  const auto& m = as<sketch::Moments>(st);
  return m.n >= 2 ? number_or_null(m.stddev()) : Value::null();
}
Value variance_result(const void* st, const AggSpec&, const ResultContext&) {
  const auto& m = as<sketch::Moments>(st);
  return m.n >= 2 ? number_or_null(m.variance()) : Value::null();
}

// ---------------------------------------------------------------- min / max

struct Extreme {
  double v = 0;
  bool has = false;
};
void min_update(void* st, const Value& v, int64_t, const AggSpec&) {
  double x;
  if (!zql::to_number(v, x) || x != x) return;
  Extreme& e = as<Extreme>(st);
  if (!e.has || x < e.v) {
    e.v = x;
    e.has = true;
  }
}
void max_update(void* st, const Value& v, int64_t, const AggSpec&) {
  double x;
  if (!zql::to_number(v, x) || x != x) return;
  Extreme& e = as<Extreme>(st);
  if (!e.has || x > e.v) {
    e.v = x;
    e.has = true;
  }
}
void min_merge(void* d, const void* s, const AggSpec&) {
  const Extreme& b = as<Extreme>(s);
  Extreme& a = as<Extreme>(d);
  if (b.has && (!a.has || b.v < a.v)) a = b;
}
void max_merge(void* d, const void* s, const AggSpec&) {
  const Extreme& b = as<Extreme>(s);
  Extreme& a = as<Extreme>(d);
  if (b.has && (!a.has || b.v > a.v)) a = b;
}
Value extreme_result(const void* st, const AggSpec&, const ResultContext&) {
  const Extreme& e = as<Extreme>(st);
  return e.has ? Value::number(e.v) : Value::null();
}

// ---------------------------------------------------------------- sketches (punteros)

template <class S>
struct Boxed {
  S* p = nullptr;
};

template <class S>
void boxed_init(void* st) {
  ::new (st) Boxed<S>();
}
template <class S>
void boxed_destroy(void* st) {
  delete as<Boxed<S>>(st).p;
}
template <class S>
void boxed_copy(void* dst, const void* src) {
  const S* s = as<Boxed<S>>(src).p;
  ::new (dst) Boxed<S>{s ? new S(*s) : nullptr};
}

void quantile_update(void* st, const Value& v, int64_t, const AggSpec&) {
  double x;
  if (!zql::to_number(v, x) || !std::isfinite(x)) return;
  auto& b = as<Boxed<sketch::DDSketch>>(st);
  if (!b.p) b.p = new sketch::DDSketch();
  b.p->add(x);
}
void quantile_merge(void* d, const void* s, const AggSpec&) {
  const auto* src = as<Boxed<sketch::DDSketch>>(s).p;
  if (!src) return;
  auto& dst = as<Boxed<sketch::DDSketch>>(d);
  if (!dst.p)
    dst.p = new sketch::DDSketch(*src);
  else
    dst.p->merge(*src);
}
// Redondea a `digits` cifras significativas. El sketch garantiza un 1 % de
// error relativo: publicar 16 dígitos sería precisión falsa, y 4 cifras
// añaden como mucho un 0,05 % más. Es monótono, así que no desordena cuantiles.
double round_significant(double x, int digits) noexcept {
  if (x == 0 || !std::isfinite(x)) return x;
  const int exponent = static_cast<int>(std::floor(std::log10(std::fabs(x))));
  const int shift = digits - 1 - exponent;
  // Las potencias de 10 hasta 1e22 son exactas en double: se divide o
  // multiplica por una exacta para que el resultado sea el double más cercano.
  if (shift >= 0) {
    const double scale = std::pow(10.0, shift);
    return std::round(x * scale) / scale;
  }
  const double scale = std::pow(10.0, -shift);
  return std::round(x / scale) * scale;
}

Value quantile_result(const void* st, const AggSpec& spec, const ResultContext&) {
  const auto* s = as<Boxed<sketch::DDSketch>>(st).p;
  return s ? number_or_null(round_significant(s->quantile(spec.q), 4)) : Value::null();
}

void distinct_update(void* st, const Value& v, int64_t, const AggSpec&) {
  if (v.is_null()) return;
  auto& b = as<Boxed<sketch::HyperLogLog>>(st);
  if (!b.p) b.p = new sketch::HyperLogLog();
  const std::string& e = encode(v);
  b.p->add_hash(hash_bytes(e.data(), e.size()));
}
void distinct_merge(void* d, const void* s, const AggSpec&) {
  const auto* src = as<Boxed<sketch::HyperLogLog>>(s).p;
  if (!src) return;
  auto& dst = as<Boxed<sketch::HyperLogLog>>(d);
  if (!dst.p)
    dst.p = new sketch::HyperLogLog(*src);
  else
    dst.p->merge(*src);
}
Value distinct_result(const void* st, const AggSpec&, const ResultContext&) {
  const auto* s = as<Boxed<sketch::HyperLogLog>>(st).p;
  return Value::number(s ? std::round(s->estimate()) : 0.0);
}

void topk_update(void* st, const Value& v, int64_t, const AggSpec& spec) {
  if (v.is_null()) return;
  auto& b = as<Boxed<sketch::SpaceSaving>>(st);
  if (!b.p) b.p = new sketch::SpaceSaving(topk_capacity(spec.k));
  b.p->add(encode(v));
}
void topk_merge(void* d, const void* s, const AggSpec& spec) {
  const auto* src = as<Boxed<sketch::SpaceSaving>>(s).p;
  if (!src) return;
  auto& dst = as<Boxed<sketch::SpaceSaving>>(d);
  if (!dst.p) dst.p = new sketch::SpaceSaving(topk_capacity(spec.k));
  dst.p->merge(*src);
}
Value topk_result(const void* st, const AggSpec& spec, const ResultContext& ctx) {
  const auto* s = as<Boxed<sketch::SpaceSaving>>(st).p;
  std::string out;
  json::Writer w(out);
  w.begin_array();
  if (s) {
    for (const auto& e : s->top(spec.k)) {
      size_t pos = 0;
      Value item;
      if (!zql::decode_value(e.item, pos, item)) continue;
      w.begin_object().key("value");
      std::string tmp;
      zql::write_json(tmp, item);
      w.raw(tmp);
      w.kv_int("count", static_cast<int64_t>(e.count)).end_object();
    }
  }
  w.end_array();
  return Value::json(ctx.scratch->copy(out));
}

// ---------------------------------------------------------------- first / last

struct Timed {
  int64_t ts = 0;
  bool has = false;
  std::string enc;
};
void timed_init(void* st) { ::new (st) Timed(); }
void timed_destroy(void* st) { as<Timed>(st).~Timed(); }
void timed_copy(void* dst, const void* src) { ::new (dst) Timed(as<Timed>(src)); }

void first_update(void* st, const Value& v, int64_t ts, const AggSpec&) {
  if (v.is_null()) return;
  Timed& t = as<Timed>(st);
  if (!t.has || ts < t.ts) {
    t.ts = ts;
    t.has = true;
    t.enc = encode(v);
  }
}
void last_update(void* st, const Value& v, int64_t ts, const AggSpec&) {
  if (v.is_null()) return;
  Timed& t = as<Timed>(st);
  if (!t.has || ts >= t.ts) {
    t.ts = ts;
    t.has = true;
    t.enc = encode(v);
  }
}
void first_merge(void* d, const void* s, const AggSpec&) {
  const Timed& b = as<Timed>(s);
  Timed& a = as<Timed>(d);
  if (b.has && (!a.has || b.ts < a.ts)) a = b;
}
void last_merge(void* d, const void* s, const AggSpec&) {
  const Timed& b = as<Timed>(s);
  Timed& a = as<Timed>(d);
  if (b.has && (!a.has || b.ts >= a.ts)) a = b;
}
Value timed_result(const void* st, const AggSpec&, const ResultContext& ctx) {
  const Timed& t = as<Timed>(st);
  if (!t.has) return Value::null();
  const std::string_view copy = ctx.scratch->copy(t.enc);
  size_t pos = 0;
  Value v;
  return zql::decode_value(copy, pos, v) ? v : Value::null();
}

// ---------------------------------------------------------------- tabla

constexpr AggOps kCount{sizeof(uint64_t), alignof(uint64_t), pod_init<uint64_t>, pod_destroy, count_update,
                        count_merge, pod_copy<uint64_t>, count_result};
constexpr AggOps kCountValues{sizeof(uint64_t), alignof(uint64_t), pod_init<uint64_t>, pod_destroy, count_values_update,
                              count_merge, pod_copy<uint64_t>, count_result};
constexpr AggOps kSum{sizeof(KSum), alignof(KSum), pod_init<KSum>, pod_destroy, sum_update,
                      sum_merge, pod_copy<KSum>, sum_result};
constexpr AggOps kRate{sizeof(KSum), alignof(KSum), pod_init<KSum>, pod_destroy, rate_update,
                       sum_merge, pod_copy<KSum>, rate_result};
constexpr AggOps kAvg{sizeof(sketch::Moments), alignof(sketch::Moments), pod_init<sketch::Moments>, pod_destroy,
                      moments_update, moments_merge, pod_copy<sketch::Moments>, avg_result};
constexpr AggOps kStddev{sizeof(sketch::Moments), alignof(sketch::Moments), pod_init<sketch::Moments>, pod_destroy,
                         moments_update, moments_merge, pod_copy<sketch::Moments>, stddev_result};
constexpr AggOps kVariance{sizeof(sketch::Moments), alignof(sketch::Moments), pod_init<sketch::Moments>, pod_destroy,
                           moments_update, moments_merge, pod_copy<sketch::Moments>, variance_result};
constexpr AggOps kMin{sizeof(Extreme), alignof(Extreme), pod_init<Extreme>, pod_destroy, min_update,
                      min_merge, pod_copy<Extreme>, extreme_result};
constexpr AggOps kMax{sizeof(Extreme), alignof(Extreme), pod_init<Extreme>, pod_destroy, max_update,
                      max_merge, pod_copy<Extreme>, extreme_result};
constexpr AggOps kQuantile{sizeof(Boxed<sketch::DDSketch>), alignof(Boxed<sketch::DDSketch>),
                           boxed_init<sketch::DDSketch>, boxed_destroy<sketch::DDSketch>, quantile_update,
                           quantile_merge, boxed_copy<sketch::DDSketch>, quantile_result};
constexpr AggOps kDistinct{sizeof(Boxed<sketch::HyperLogLog>), alignof(Boxed<sketch::HyperLogLog>),
                           boxed_init<sketch::HyperLogLog>, boxed_destroy<sketch::HyperLogLog>, distinct_update,
                           distinct_merge, boxed_copy<sketch::HyperLogLog>, distinct_result};
constexpr AggOps kTopK{sizeof(Boxed<sketch::SpaceSaving>), alignof(Boxed<sketch::SpaceSaving>),
                       boxed_init<sketch::SpaceSaving>, boxed_destroy<sketch::SpaceSaving>, topk_update,
                       topk_merge, boxed_copy<sketch::SpaceSaving>, topk_result};
constexpr AggOps kFirst{sizeof(Timed), alignof(Timed), timed_init, timed_destroy, first_update,
                        first_merge, timed_copy, timed_result};
constexpr AggOps kLast{sizeof(Timed), alignof(Timed), timed_init, timed_destroy, last_update,
                       last_merge, timed_copy, timed_result};

}  // namespace

const AggOps& agg_ops(AggKind kind) noexcept {
  switch (kind) {
    case AggKind::Count: return kCount;
    case AggKind::CountValues: return kCountValues;
    case AggKind::Sum: return kSum;
    case AggKind::Avg: return kAvg;
    case AggKind::Min: return kMin;
    case AggKind::Max: return kMax;
    case AggKind::Stddev: return kStddev;
    case AggKind::Variance: return kVariance;
    case AggKind::Quantile: return kQuantile;
    case AggKind::Distinct: return kDistinct;
    case AggKind::TopK: return kTopK;
    case AggKind::First: return kFirst;
    case AggKind::Last: return kLast;
    case AggKind::Rate: return kRate;
  }
  return kCount;
}

AggLayout::AggLayout(std::shared_ptr<const zql::QueryPlan> plan) : plan_(std::move(plan)) {
  uint32_t off = 0;
  for (const auto& spec : plan_->aggs) {
    const AggOps& ops = agg_ops(spec.kind);
    off = (off + ops.align - 1) & ~(ops.align - 1);
    entries_.push_back({&ops, off, &spec});
    off += ops.size;
    block_align_ = std::max(block_align_, ops.align);
  }
  block_size_ = std::max<uint32_t>(off, 1);
  block_size_ = (block_size_ + block_align_ - 1) & ~(block_align_ - 1);
}

void AggLayout::init_block(char* block) const {
  for (const auto& e : entries_) e.ops->init(block + e.offset);
}

void AggLayout::destroy_block(char* block) const {
  for (const auto& e : entries_) e.ops->destroy(block + e.offset);
}

void AggLayout::copy_block(char* dst, const char* src) const {
  for (const auto& e : entries_) e.ops->copy(dst + e.offset, src + e.offset);
}

void AggLayout::merge_block(char* dst, const char* src) const {
  for (const auto& e : entries_) e.ops->merge(dst + e.offset, src + e.offset, *e.spec);
}

}  // namespace zr::engine
