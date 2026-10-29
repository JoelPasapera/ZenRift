#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "framework.hpp"
#include "temp_dir.hpp"
#include "storage/partition_log.hpp"

using namespace zr;
using namespace zr::storage;
using zt::TempDir;
namespace fs = std::filesystem;

namespace {

std::string payload(int i) { return "{\"i\":" + std::to_string(i) + ",\"pad\":\"" + std::string(i % 50, 'x') + "\"}"; }

std::vector<LogRecord> read_all(PartitionLog& log, std::string& scratch, int64_t from = 0) {
  LogCursor c;
  c.offset = from;
  std::vector<LogRecord> all, batch;
  std::string err;
  std::vector<std::string> keep;
  while (true) {
    REQUIRE(log.read(c, 37, 1 << 20, scratch, batch, err));
    if (batch.empty()) break;
    for (auto& r : batch) {
      keep.emplace_back(r.payload);
      all.push_back(r);
    }
  }
  // Re-apuntar las vistas a copias estables.
  static std::vector<std::string> storage;
  storage = std::move(keep);
  for (size_t i = 0; i < all.size(); ++i) all[i].payload = storage[i];
  return all;
}

}  // namespace

TEST(log_anexa_lee_y_reabre) {
  TempDir dir;
  std::string err, scratch;
  {
    PartitionLog log;
    REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
    for (int i = 0; i < 1000; ++i) {
      int64_t off;
      REQUIRE(log.append(1000 + i, payload(i), off, err));
      CHECK_EQ(off, int64_t{i});
    }
    REQUIRE(log.sync(err));
    const auto all = read_all(log, scratch);
    REQUIRE(all.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
      CHECK_EQ(all[i].offset, int64_t{i});
      CHECK_EQ(all[i].ts, int64_t{1000 + i});
      CHECK_EQ(std::string(all[i].payload), payload(i));
    }
  }
  PartitionLog log;
  REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
  CHECK_EQ(log.end_offset(), int64_t{1000});
  CHECK_EQ(log.recovery().records, uint64_t{1000});
  CHECK(log.recovery().warnings.empty());
  int64_t off;
  REQUIRE(log.append(5000, "{}", off, err));
  CHECK_EQ(off, int64_t{1000});
  const auto all = read_all(log, scratch, 995);
  REQUIRE(all.size() == 6);
  CHECK_EQ(std::string(all.back().payload), std::string("{}"));
}

TEST(log_rota_segmentos_y_lee_a_traves_de_ellos) {
  TempDir dir;
  std::string err, scratch;
  LogOptions opts;
  opts.segment_bytes = 4096;
  opts.index_interval = 256;
  PartitionLog log;
  REQUIRE(log.open(dir.path.string(), opts, err));
  for (int i = 0; i < 2000; ++i) {
    int64_t off;
    REQUIRE(log.append(i, payload(i), off, err));
  }
  CHECK(log.segment_count() > 20);
  // Lectura desde offsets arbitrarios (usa el índice disperso de cada segmento).
  for (int64_t from : {0, 1, 63, 500, 1234, 1999}) {
    LogCursor c;
    c.offset = from;
    std::vector<LogRecord> batch;
    REQUIRE(log.read(c, 5, 1 << 20, scratch, batch, err));
    REQUIRE(!batch.empty());
    CHECK_EQ(batch.front().offset, from);
    CHECK_EQ(std::string(batch.front().payload), payload(static_cast<int>(from)));
  }
  const auto all = read_all(log, scratch);
  CHECK_EQ(all.size(), size_t{2000});
  // Reabrir conserva todo.
  log.close();
  PartitionLog again;
  REQUIRE(again.open(dir.path.string(), opts, err));
  CHECK_EQ(again.end_offset(), int64_t{2000});
  CHECK_EQ(read_all(again, scratch).size(), size_t{2000});
}

TEST(log_cursor_sigue_al_segmento_activo) {
  TempDir dir;
  std::string err, scratch;
  PartitionLog log;
  REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
  LogCursor c;
  std::vector<LogRecord> batch;
  int64_t off;
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 10; ++i) REQUIRE(log.append(round * 10 + i, payload(round * 10 + i), off, err));
    REQUIRE(log.read(c, 1000, 1 << 20, scratch, batch, err));
    REQUIRE(batch.size() == 10);
    CHECK_EQ(batch.front().offset, int64_t{round * 10});
    CHECK_EQ(c.offset, int64_t{round * 10 + 10});
  }
  REQUIRE(log.read(c, 1000, 1 << 20, scratch, batch, err));
  CHECK(batch.empty());
}

TEST(log_recupera_cola_rota_tras_caida) {
  TempDir dir;
  std::string err, scratch;
  std::string seg_path;
  {
    PartitionLog log;
    REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
    int64_t off;
    for (int i = 0; i < 100; ++i) REQUIRE(log.append(i, payload(i), off, err));
    REQUIRE(log.sync(err));
    seg_path = (dir.path / "00000000000000000000.log").string();
  }
  // Simular un registro a medio escribir: cabecera que promete 500 bytes y solo 10.
  {
    std::ofstream f(seg_path, std::ios::binary | std::ios::app);
    const uint32_t len = 500, crc = 0;
    const int64_t ts = 7;
    f.write(reinterpret_cast<const char*>(&len), 4);
    f.write(reinterpret_cast<const char*>(&crc), 4);
    f.write(reinterpret_cast<const char*>(&ts), 8);
    f.write("0123456789", 10);
  }
  const auto before = fs::file_size(seg_path);
  PartitionLog log;
  REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
  CHECK_EQ(log.end_offset(), int64_t{100});
  CHECK_EQ(log.recovery().truncated_bytes, uint64_t{26});
  CHECK_EQ(log.recovery().warnings.size(), size_t{1});
  CHECK_EQ(fs::file_size(seg_path), before - 26);
  int64_t off;
  REQUIRE(log.append(100, payload(100), off, err));
  CHECK_EQ(off, int64_t{100});
  CHECK_EQ(read_all(log, scratch).size(), size_t{101});
}

TEST(log_detecta_corrupcion_por_crc) {
  TempDir dir;
  std::string err, scratch;
  const std::string seg_path = (dir.path / "00000000000000000000.log").string();
  uint64_t size_after_50 = 0;
  {
    PartitionLog log;
    REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
    int64_t off;
    for (int i = 0; i < 50; ++i) REQUIRE(log.append(i, payload(i), off, err));
    REQUIRE(log.flush(err));
    size_after_50 = fs::file_size(seg_path);
    for (int i = 50; i < 80; ++i) REQUIRE(log.append(i, payload(i), off, err));
    REQUIRE(log.sync(err));
  }
  // Voltear un bit dentro del payload del registro 50.
  {
    std::fstream f(seg_path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(size_after_50 + 16 + 3));
    f.put('Z');
  }
  PartitionLog log;
  REQUIRE(log.open(dir.path.string(), LogOptions{}, err));
  CHECK_EQ(log.end_offset(), int64_t{50});  // se descarta desde el registro dañado
  REQUIRE(!log.recovery().warnings.empty());
  CHECK(log.recovery().warnings[0].find("CRC") != std::string::npos);
}

TEST(log_retencion_por_tamano_y_edad_y_busqueda_por_tiempo) {
  TempDir dir;
  std::string err, scratch;
  LogOptions opts;
  opts.segment_bytes = 2048;
  PartitionLog log;
  REQUIRE(log.open(dir.path.string(), opts, err));
  int64_t off;
  for (int i = 0; i < 1000; ++i) REQUIRE(log.append(1000000 + i * 1000, payload(i), off, err));
  const size_t segs = log.segment_count();
  // Buscar por tiempo: el segmento que contiene ts = 1000000 + 600*1000.
  const int64_t o = log.offset_for_time(1000000 + 600 * 1000);
  CHECK(o <= 600);
  CHECK(o > 500);
  // Por edad: todo lo anterior a 1000000 + 500*1000.
  const size_t removed = log.enforce_retention(1000000 + 500 * 1000 + 3600000, 0, 3600000);
  CHECK(removed > 0);
  CHECK(log.start_offset() > 400);
  CHECK(log.start_offset() <= 500);
  // Por tamaño: quedarse con ~8 KB.
  log.enforce_retention(0, 8192, 0);
  CHECK(log.size_bytes() <= 8192 + 2048);
  CHECK(log.segment_count() < segs);
  // Un cursor que apuntaba a datos borrados salta al inicio disponible.
  LogCursor c;
  std::vector<LogRecord> batch;
  REQUIRE(log.read(c, 1, 1 << 20, scratch, batch, err));
  REQUIRE(batch.size() == 1);
  CHECK_EQ(batch[0].offset, log.start_offset());
  CHECK_EQ(log.end_offset(), int64_t{1000});
}

TEST(log_destroy_borra_la_particion) {
  TempDir dir;
  std::string err;
  PartitionLog log;
  REQUIRE(log.open((dir.path / "p0").string(), LogOptions{}, err));
  int64_t off;
  REQUIRE(log.append(1, "{}", off, err));
  REQUIRE(log.destroy(err));
  CHECK(!fs::exists(dir.path / "p0"));
}
