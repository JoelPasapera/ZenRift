// Log de solo-anexado de una partición, dividido en segmentos.
//
//   <dir>/00000000000000000000.log   segmentos nombrados por su offset base
//   <dir>/00000000000000524288.log
//
// Cada segmento empieza con una cabecera "ZRLOGv1\n" y contiene registros:
//
//   u32 longitud | u32 crc32c(ts + payload) | i64 ts_ms | payload (JSON)
//
// Al abrir se recorre cada segmento verificando los CRC: el índice disperso se
// reconstruye y una cola rota por una caída (registro a medio escribir) se
// trunca. Los offsets son implícitos (base del segmento + posición), así que
// no hace falta ningún archivo de índice que pueda desincronizarse.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "storage/file.hpp"

namespace zr::storage {

struct LogOptions {
  uint64_t segment_bytes = 64ull << 20;   // tamaño al que se rota el segmento activo
  uint64_t write_buffer = 256u << 10;     // búfer de escritura en espacio de usuario
  uint32_t index_interval = 4096;         // una entrada de índice cada ~4 KB
  uint32_t max_record_bytes = 16u << 20;  // límite de cordura al recuperar
};

struct LogRecord {
  int64_t offset;
  int64_t ts;
  std::string_view payload;
};

struct RecoveryReport {
  size_t segments = 0;
  uint64_t records = 0;
  uint64_t truncated_bytes = 0;  // bytes descartados por colas rotas o corruptas
  std::vector<std::string> warnings;
};

// Posición de lectura reutilizable entre llamadas (evita buscar en el índice).
struct LogCursor {
  int64_t offset = 0;
  int64_t segment_base = -1;
  uint64_t file_pos = 0;
};

class PartitionLog {
 public:
  PartitionLog();
  ~PartitionLog();
  PartitionLog(const PartitionLog&) = delete;
  PartitionLog& operator=(const PartitionLog&) = delete;

  bool open(const std::string& dir, const LogOptions& opts, std::string& err);
  void close();

  // Añade un registro; `offset` recibe el offset asignado.
  bool append(int64_t ts, std::string_view payload, int64_t& offset, std::string& err);
  // Pasa el búfer al sistema operativo (sobrevive a la caída del proceso).
  bool flush(std::string& err);
  // flush + fsync del segmento activo (sobrevive a un corte de luz).
  bool sync(std::string& err);

  // Lee hasta `max_records` registros (y ~`max_bytes`) desde cursor.offset.
  // Las vistas de `out` apuntan a `scratch`. Avanza el cursor.
  bool read(LogCursor& cursor, size_t max_records, size_t max_bytes, std::string& scratch,
            std::vector<LogRecord>& out, std::string& err);

  // Primer offset de un segmento que puede contener eventos con ts >= t.
  int64_t offset_for_time(int64_t t) const;

  // Borra segmentos antiguos (nunca el activo). Devuelve cuántos borró.
  size_t enforce_retention(int64_t now_ms, uint64_t max_bytes, int64_t max_age_ms);

  // Cierra y borra todos los archivos de la partición.
  bool destroy(std::string& err);

  int64_t start_offset() const noexcept;
  int64_t end_offset() const noexcept { return next_offset_; }
  uint64_t size_bytes() const noexcept;
  size_t segment_count() const noexcept { return segments_.size(); }
  int64_t min_ts() const noexcept;
  int64_t max_ts() const noexcept;
  const RecoveryReport& recovery() const noexcept { return recovery_; }
  const std::string& dir() const noexcept { return dir_; }

 private:
  struct Segment;

  bool open_segment(int64_t base, std::string& err);
  bool recover_segment(Segment& seg, std::string& err);
  bool roll(std::string& err);
  Segment* find_segment(int64_t offset) const;
  bool locate(Segment& seg, int64_t offset, uint64_t& pos, std::string& err);

  std::string dir_;
  LogOptions opts_;
  std::vector<std::unique_ptr<Segment>> segments_;  // ordenados por base
  int64_t next_offset_ = 0;
  std::string wbuf_;
  RecoveryReport recovery_;
};

}  // namespace zr::storage
