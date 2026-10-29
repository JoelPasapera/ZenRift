#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "core/arena.hpp"
#include "core/crc32c.hpp"
#include "core/flat_map.hpp"
#include "core/hash.hpp"
#include "core/mpsc_queue.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "framework.hpp"

using namespace zr;

TEST(hash_es_determinista_y_sensible_a_cada_byte) {
  const std::string a = "requests.latency_ms";
  CHECK_EQ(hash_str(a), hash_str(a));
  std::set<uint64_t> seen;
  for (size_t len = 0; len < 100; ++len) {
    std::string s(len, 'x');
    seen.insert(hash_str(s));
    if (len > 0) {
      s[len / 2] = 'y';
      seen.insert(hash_str(s));
    }
  }
  CHECK_EQ(seen.size(), size_t{199});
  CHECK(hash_str("a", 1) != hash_str("a", 2));
}

TEST(hash_distribuye_bits_uniformemente) {
  // Cada bit de salida debe ponerse a 1 cerca del 50 % de las veces.
  int ones[64] = {};
  const int n = 20000;
  for (int i = 0; i < n; ++i) {
    const std::string k = "user-" + std::to_string(i);
    const uint64_t h = hash_str(k);
    for (int b = 0; b < 64; ++b) ones[b] += (h >> b) & 1;
  }
  for (int b = 0; b < 64; ++b) CHECK(ones[b] > n * 0.47 && ones[b] < n * 0.53);
}

TEST(flat_map_inserta_busca_y_borra) {
  FlatMap<uint64_t, int> m;
  for (uint64_t i = 0; i < 10000; ++i) {
    auto [v, inserted] = m.try_emplace(i * 7919, static_cast<int>(i));
    CHECK(inserted);
    CHECK_EQ(*v, static_cast<int>(i));
  }
  CHECK_EQ(m.size(), size_t{10000});
  for (uint64_t i = 0; i < 10000; ++i) {
    const int* v = m.find(i * 7919);
    REQUIRE(v != nullptr);
    CHECK_EQ(*v, static_cast<int>(i));
  }
  CHECK(m.find(3) == nullptr);
  auto [v2, ins2] = m.try_emplace(7919 * 5, 99);
  CHECK(!ins2);
  CHECK_EQ(*v2, 5);
  for (uint64_t i = 0; i < 10000; i += 2) CHECK(m.erase(i * 7919));
  CHECK_EQ(m.size(), size_t{5000});
  for (uint64_t i = 0; i < 10000; ++i) CHECK_EQ(m.contains(i * 7919), i % 2 == 1);
  CHECK(!m.erase(0));
}

TEST(flat_map_coincide_con_unordered_map_en_operaciones_aleatorias) {
  FlatMap<std::string_view, std::string> m;
  std::unordered_map<std::string, std::string> ref;
  std::vector<std::string> keys;
  for (int i = 0; i < 2000; ++i) keys.push_back("k" + std::to_string(i * 31 % 1777));
  std::mt19937_64 rng(42);
  for (int step = 0; step < 50000; ++step) {
    const std::string& k = keys[rng() % keys.size()];
    const int op = static_cast<int>(rng() % 3);
    if (op == 0) {
      m[k] = std::to_string(step);
      ref[k] = std::to_string(step);
    } else if (op == 1) {
      CHECK_EQ(m.erase(k), ref.erase(k) == 1);
    } else {
      const std::string* v = m.find(k);
      auto it = ref.find(k);
      CHECK_EQ(v != nullptr, it != ref.end());
      if (v && it != ref.end()) CHECK_EQ(*v, it->second);
    }
  }
  CHECK_EQ(m.size(), ref.size());
  FlatMap<std::string_view, std::string> copy(m);
  CHECK_EQ(copy.size(), m.size());
  size_t visited = 0;
  copy.for_each([&](std::string_view k, const std::string& v) {
    ++visited;
    CHECK_EQ(ref.at(std::string(k)), v);
  });
  CHECK_EQ(visited, ref.size());
}

TEST(arena_alinea_y_reutiliza_bloques) {
  Arena a(256);
  auto* p1 = static_cast<char*>(a.alloc(3, 1));
  auto* p2 = a.alloc_array<double>(4);
  CHECK(reinterpret_cast<uintptr_t>(p2) % alignof(double) == 0);
  CHECK(p1 != nullptr);
  const std::string_view s = a.copy("hola mundo");
  CHECK_EQ(s, std::string_view("hola mundo"));
  auto* big = static_cast<char*>(a.alloc(10000, 8));
  big[9999] = 'x';
  const size_t reserved = a.bytes_reserved();
  a.reset();
  CHECK_EQ(a.bytes_used(), size_t{0});
  for (int i = 0; i < 10; ++i) a.alloc(100, 8);
  CHECK(a.bytes_reserved() <= reserved + 256);
}

TEST(rfc3339_analiza_y_formatea) {
  int64_t ms = 0;
  REQUIRE(parse_rfc3339("2026-10-29T13:45:00.123Z", ms));
  CHECK_EQ(format_rfc3339(ms), std::string("2026-10-29T13:45:00.123Z"));
  REQUIRE(parse_rfc3339("2026-10-29T08:45:00-05:00", ms));
  CHECK_EQ(format_rfc3339(ms), std::string("2026-10-29T13:45:00.000Z"));
  REQUIRE(parse_rfc3339("1970-01-01T00:00:00Z", ms));
  CHECK_EQ(ms, 0);
  REQUIRE(parse_rfc3339("2000-02-29", ms));
  CHECK_EQ(format_rfc3339(ms), std::string("2000-02-29T00:00:00.000Z"));
  REQUIRE(parse_rfc3339("2026-10-29 13:45:00.1234567+0100", ms));
  CHECK_EQ(format_rfc3339(ms), std::string("2026-10-29T12:45:00.123Z"));
  CHECK(!parse_rfc3339("2026-02-30T00:00:00Z", ms));
  CHECK(!parse_rfc3339("2026-10-29T25:00:00Z", ms));
  CHECK(!parse_rfc3339("2026-10-29T13:45:00Zx", ms));
  CHECK(!parse_rfc3339("hola", ms));
  CHECK_EQ(format_rfc3339(-1), std::string("1969-12-31T23:59:59.999Z"));
}

TEST(fechas_civiles_ida_y_vuelta) {
  for (int64_t d = -800000; d < 800000; d += 997) {
    int64_t y;
    unsigned m, dd;
    civil_from_days(d, y, m, dd);
    CHECK_EQ(days_from_civil(y, m, dd), d);
  }
}

TEST(duraciones) {
  int64_t ms = 0;
  CHECK(parse_duration("10s", ms) && ms == 10000);
  CHECK(parse_duration("500ms", ms) && ms == 500);
  CHECK(parse_duration("1h30m", ms) && ms == 5400000);
  CHECK(parse_duration("1.5s", ms) && ms == 1500);
  CHECK(parse_duration("2d", ms) && ms == 172800000);
  CHECK(!parse_duration("10", ms));
  CHECK(!parse_duration("10x", ms));
  CHECK(!parse_duration("", ms));
  CHECK_EQ(format_duration(5400000), std::string("1h30m"));
  CHECK_EQ(format_duration(10000), std::string("10s"));
  CHECK_EQ(format_duration(250), std::string("250ms"));
  CHECK_EQ(floor_div(-1, 10), int64_t{-1});
  CHECK_EQ(align_down(-1, 10), int64_t{-10});
  CHECK_EQ(align_down(19, 10), int64_t{10});
}

TEST(epoca_numerica_adivina_unidad) {
  CHECK_EQ(epoch_number_to_ms(1761760000.5), int64_t{1761760000500});
  CHECK_EQ(epoch_number_to_ms(1761760000123.0), int64_t{1761760000123});
  CHECK_EQ(epoch_number_to_ms(1761760000123456.0), int64_t{1761760000123});
  CHECK_EQ(epoch_number_to_ms(1761760000123456789.0) / 1000, int64_t{1761760000});
}

TEST(crc32c_vectores_conocidos) {
  CHECK_EQ(crc32c("123456789", 9), 0xE3069283u);
  CHECK_EQ(crc32c_sw("123456789", 9), 0xE3069283u);
  CHECK_EQ(crc32c("", 0), 0u);
  // 32 bytes a cero (vector de iSCSI, RFC 3720).
  const std::string zeros(32, '\0');
  CHECK_EQ(crc32c(zeros.data(), zeros.size()), 0x8A9136AAu);
  std::string ones(32, '\xff');
  CHECK_EQ(crc32c(ones.data(), ones.size()), 0x62A8AB43u);
  // Hardware y software coinciden, y el CRC es encadenable.
  std::mt19937 rng(7);
  std::string data(10007, '\0');
  for (auto& c : data) c = static_cast<char>(rng());
  const uint32_t whole = crc32c(data.data(), data.size());
  CHECK_EQ(crc32c_sw(data.data(), data.size()), whole);
  const uint32_t part = crc32c(data.data(), 3333);
  CHECK_EQ(crc32c(data.data() + 3333, data.size() - 3333, part), whole);
}

TEST(cadenas_numeros_y_tamanos) {
  std::string s;
  append_double(s, 3.0);
  s += ' ';
  append_double(s, 0.1);
  s += ' ';
  append_double(s, -0.0);
  s += ' ';
  append_double(s, 1e300);
  s += ' ';
  append_double(s, std::nan(""));
  CHECK_EQ(s, std::string("3 0.1 0 1e+300 null"));
  uint64_t n = 0;
  CHECK(parse_size("64MB", n) && n == 64ull * 1024 * 1024);
  CHECK(parse_size("1.5 GB", n) && n == 1610612736ull);
  CHECK(parse_size("4096", n) && n == 4096);
  CHECK(!parse_size("12 parsecs", n));
  CHECK_EQ(format_bytes(1536), std::string("1.5 KB"));
  std::string e;
  append_json_escaped(e, "a\"b\\c\n\x01");
  CHECK_EQ(e, std::string("a\\\"b\\\\c\\n\\u0001"));
  CHECK_EQ(url_decode("a%20b+c%2Fd%zz"), std::string("a b c/d%zz"));
  CHECK_EQ(cat("x=", 3, " y=", 2.5, ' ', true), std::string("x=3 y=2.5 true"));
}

TEST(cola_mpsc_entrega_todo_entre_hilos) {
  MpscQueue<int> q;
  constexpr int kProducers = 4, kPer = 20000;
  std::vector<std::thread> ts;
  for (int p = 0; p < kProducers; ++p)
    ts.emplace_back([&, p] {
      for (int i = 0; i < kPer; ++i) q.push(p * kPer + i);
    });
  std::vector<int> batch;
  std::vector<int> last(kProducers, -1);
  int received = 0;
  bool ordered = true;
  while (received < kProducers * kPer) {
    q.wait_pop_all(batch, std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    for (int v : batch) {
      const int p = v / kPer;
      if (v <= last[p]) ordered = false;  // FIFO por productor
      last[p] = v;
      ++received;
    }
  }
  for (auto& t : ts) t.join();
  CHECK(ordered);
  CHECK_EQ(received, kProducers * kPer);
  CHECK(!q.try_pop_all(batch));
}
