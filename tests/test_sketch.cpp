#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/hash.hpp"
#include "framework.hpp"
#include "sketch/ddsketch.hpp"
#include "sketch/hyperloglog.hpp"
#include "sketch/moments.hpp"
#include "sketch/space_saving.hpp"

using namespace zr;
using namespace zr::sketch;

namespace {

// Verifica la garantía de DDSketch: el cuantil estimado está a menos de α
// (relativo) de alguno de los dos elementos que rodean el rango exacto.
void check_quantiles(const DDSketch& s, std::vector<double> values, double alpha) {
  std::sort(values.begin(), values.end());
  const double qs[] = {0.0, 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 0.99, 0.999, 1.0};
  for (double q : qs) {
    const double rank = q * static_cast<double>(values.size() - 1);
    const double lo = values[static_cast<size_t>(std::floor(rank))];
    const double hi = values[static_cast<size_t>(std::ceil(rank))];
    const double est = s.quantile(q);
    const double tol = alpha * 1.0000001;
    const bool ok = std::fabs(est - lo) <= tol * std::fabs(lo) + 1e-12 ||
                    std::fabs(est - hi) <= tol * std::fabs(hi) + 1e-12 || (est >= std::min(lo, hi) && est <= std::max(lo, hi));
    if (!ok)
      zt::report_failure(__FILE__, __LINE__,
                         cat("cuantil ", q, ": estimado ", est, " exacto entre ", lo, " y ", hi));
  }
}

}  // namespace

TEST(ddsketch_error_relativo_acotado_en_distribuciones_variadas) {
  std::mt19937_64 rng(1);
  std::lognormal_distribution<double> logn(3.0, 1.2);    // latencias típicas
  std::uniform_real_distribution<double> uni(-500, 500);  // con negativos y ceros cercanos
  std::exponential_distribution<double> expo(0.01);
  for (int dist = 0; dist < 3; ++dist) {
    DDSketch s(0.01);
    std::vector<double> vals;
    for (int i = 0; i < 100000; ++i) {
      const double v = dist == 0 ? logn(rng) : dist == 1 ? uni(rng) : expo(rng);
      vals.push_back(v);
      s.add(v);
    }
    CHECK_EQ(s.count(), uint64_t{100000});
    check_quantiles(s, vals, 0.01);
  }
}

TEST(ddsketch_fusion_equivale_a_un_solo_sketch) {
  std::mt19937_64 rng(2);
  std::lognormal_distribution<double> logn(2.0, 1.0);
  DDSketch whole, a, b, c;
  std::vector<double> vals;
  for (int i = 0; i < 30000; ++i) {
    const double v = logn(rng) * (i % 7 == 0 ? -1 : 1);
    vals.push_back(v);
    whole.add(v);
    (i % 3 == 0 ? a : i % 3 == 1 ? b : c).add(v);
  }
  DDSketch merged;
  merged.merge(a);
  merged.merge(b);
  merged.merge(c);
  CHECK_EQ(merged.count(), whole.count());
  for (double q = 0; q <= 1.0; q += 0.05) CHECK_EQ(merged.quantile(q), whole.quantile(q));
  CHECK_NEAR(merged.sum(), whole.sum(), 1e-6 * std::fabs(whole.sum()));
  check_quantiles(merged, vals, 0.01);
}

TEST(ddsketch_disperso_pequeno_y_colapso_preserva_cuantiles_altos) {
  DDSketch tiny;
  tiny.add(5);
  CHECK_EQ(tiny.quantile(0.5), 5.0);  // un solo valor: exacto (acotado por min/max)
  CHECK(std::isnan(DDSketch().quantile(0.5)));
  // Con un tope de 64 cubetas, un rango enorme colapsa los valores bajos.
  DDSketch s(0.01, 64);
  std::vector<double> vals;
  for (int i = 1; i <= 20000; ++i) {
    const double v = std::pow(1.001, i);  // de 1 a ~4.8e8
    vals.push_back(v);
    s.add(v);
  }
  std::sort(vals.begin(), vals.end());
  const double p99 = vals[static_cast<size_t>(0.99 * (vals.size() - 1))];
  CHECK(std::fabs(s.quantile(0.99) - p99) <= 0.0101 * p99);
  CHECK(s.memory_bytes() < 2048);
}

TEST(hyperloglog_estimacion_en_todo_el_rango) {
  const uint64_t sizes[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
  for (uint64_t n : sizes) {
    HyperLogLog h;
    for (uint64_t i = 0; i < n; ++i) h.add_hash(hash_u64(i * 2654435761ull + 17));
    const double est = h.estimate();
    const double rel = std::fabs(est - static_cast<double>(n)) / static_cast<double>(n);
    // 4 desviaciones típicas (0,81 %) para n grande; casi exacto en n pequeño.
    const double tol = n < 1000 ? 0.02 : 0.033;
    if (rel > tol) zt::report_failure(__FILE__, __LINE__, cat("n=", n, " estimado=", est));
    // Repetir valores no cambia nada.
    for (uint64_t i = 0; i < std::min<uint64_t>(n, 1000); ++i) h.add_hash(hash_u64(i * 2654435761ull + 17));
    CHECK_EQ(h.estimate(), est);
  }
  CHECK_EQ(HyperLogLog().estimate(), 0.0);
}

TEST(hyperloglog_fusion_y_modo_disperso_coherentes) {
  HyperLogLog a, b, all;
  for (uint64_t i = 0; i < 50000; ++i) {
    const uint64_t h = hash_u64(i);
    (i < 30000 ? a : b).add_hash(h);
    if (i >= 20000 && i < 30000) b.add_hash(h);  // solapamiento
    all.add_hash(h);
  }
  HyperLogLog m = a;
  m.merge(b);
  CHECK_EQ(m.estimate(), all.estimate());
  // Disperso fusionado en denso y viceversa.
  HyperLogLog small1, small2;
  for (uint64_t i = 0; i < 300; ++i) small1.add_hash(hash_u64(i + 1000000));
  for (uint64_t i = 0; i < 300; ++i) small2.add_hash(hash_u64(i + 1000150));
  CHECK(small1.is_sparse());
  HyperLogLog big = all;
  big.merge(small1);
  HyperLogLog direct = all;
  for (uint64_t i = 0; i < 300; ++i) direct.add_hash(hash_u64(i + 1000000));
  CHECK_EQ(big.estimate(), direct.estimate());
  small1.merge(small2);
  CHECK(std::fabs(small1.estimate() - 450) < 10);
  CHECK(small1.memory_bytes() < 16384);
}

TEST(space_saving_encuentra_los_mas_frecuentes) {
  // Distribución de Zipf sobre 5000 elementos.
  std::mt19937_64 rng(3);
  std::vector<double> cdf;
  double acc = 0;
  for (int i = 1; i <= 5000; ++i) {
    acc += 1.0 / std::pow(i, 1.1);
    cdf.push_back(acc);
  }
  std::uniform_real_distribution<double> u(0, acc);
  SpaceSaving ss(80);
  std::map<std::string, uint64_t> exact;
  for (int i = 0; i < 200000; ++i) {
    const size_t idx = static_cast<size_t>(std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin());
    const std::string item = "ruta/" + std::to_string(idx);
    ss.add(item);
    ++exact[item];
  }
  CHECK_EQ(ss.total(), uint64_t{200000});
  const auto top = ss.top(10);
  REQUIRE(top.size() == 10);
  for (size_t i = 0; i < top.size(); ++i) {
    CHECK_EQ(top[i].item, "ruta/" + std::to_string(i));  // los 10 más probables, en orden
    const uint64_t real = exact[top[i].item];
    CHECK(top[i].count >= real);
    CHECK(top[i].count - top[i].error <= real);
  }
}

TEST(space_saving_fusion_conserva_garantias) {
  SpaceSaving a(16), b(16);
  std::map<std::string, uint64_t> exact;
  std::mt19937 rng(4);
  for (int i = 0; i < 20000; ++i) {
    // "x" domina en a, "y" en b, mucho ruido en ambos.
    const std::string item = i % 5 == 0 ? (i % 2 ? "x" : "y") : "ruido" + std::to_string(rng() % 3000);
    ((i / 10) % 2 ? a : b).add(item);
    ++exact[item];
  }
  SpaceSaving m = a;
  m.merge(b);
  CHECK_EQ(m.total(), uint64_t{20000});
  const auto top = m.top(2);
  REQUIRE(top.size() == 2);
  std::unordered_set<std::string> names{top[0].item, top[1].item};
  CHECK(names.count("x") == 1 && names.count("y") == 1);
  for (const auto& e : m.top(16)) {
    const uint64_t real = exact[e.item];
    CHECK(e.count >= real);
    CHECK(e.count - e.error <= real);
  }
}

TEST(momentos_fusion_estable) {
  std::mt19937_64 rng(5);
  std::normal_distribution<double> nd(1e6, 3.0);  // media grande: prueba la estabilidad
  Moments all, a, b;
  std::vector<double> v;
  for (int i = 0; i < 10000; ++i) {
    const double x = nd(rng);
    v.push_back(x);
    all.add(x);
    (i % 2 ? a : b).add(x);
  }
  a.merge(b);
  double mean = 0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  double ss = 0;
  for (double x : v) ss += (x - mean) * (x - mean);
  const double var = ss / static_cast<double>(v.size() - 1);
  CHECK_NEAR(all.mean, mean, 1e-6);
  CHECK_NEAR(all.variance(), var, 1e-6 * var);
  CHECK_NEAR(a.variance(), var, 1e-6 * var);
  CHECK_EQ(a.n, all.n);
}
