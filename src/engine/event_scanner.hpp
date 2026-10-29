// Escáner de eventos de la ruta caliente.
//
// Recorre un cuerpo de ingesta (NDJSON, array JSON de objetos u objeto suelto)
// en una sola pasada: valida cada evento, extrae su marca de tiempo y solo los
// campos que pide la FieldLayout, y devuelve la porción cruda de cada objeto
// para guardarla en el log tal cual (normalizando saltos de línea).
//
// No construye ningún árbol: los valores extraídos son vistas al cuerpo o, si
// la cadena tenía escapes, copias desescapadas en el arena del lote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "engine/field_layout.hpp"
#include "zql/value.hpp"

namespace zr::engine {

struct ScanLimits {
  size_t max_event_bytes = 1u << 20;
  int64_t max_future_ms = 3600000;  // eventos con ts > ingesta + esto se rechazan
  size_t max_errors_reported = 5;
};

struct ScannedEvent {
  std::string_view raw;
  int64_t ts = 0;
  bool ts_defaulted = false;
};

struct ScanReport {
  size_t accepted = 0;
  size_t rejected = 0;
  std::vector<std::string> errors;  // los primeros, con posición
};

class EventScanner {
 public:
  explicit EventScanner(ScanLimits limits = {}) : limits_(limits) {}

  // `slots` queda con events.size() * layout.size() valores (evento i en
  // [i * L, (i + 1) * L)). `ts_slot` es la ranura del campo de tiempo o -1.
  void scan_batch(std::string_view body, const FieldLayout& layout, int32_t ts_slot, int64_t ingest_ms, Arena& arena,
                  std::vector<ScannedEvent>& events, std::vector<zql::Value>& slots, ScanReport& report);

  // Extrae campos de un evento ya validado (relectura desde el log).
  bool scan_stored(std::string_view raw, const FieldLayout& layout, Arena& arena, zql::Value* slots);

  // Marca de tiempo de un valor extraído: número de época o RFC 3339.
  static bool timestamp_of(const zql::Value& v, int64_t& out_ms) noexcept;

 private:
  struct Ctx {
    const FieldLayout* layout = nullptr;
    zql::Value* slots = nullptr;
    Arena* arena = nullptr;
    const char* err_at = nullptr;
    const char* err_msg = nullptr;
  };

  bool parse_object(const char*& p, const char* end, int depth, Ctx& c);
  bool parse_array(const char*& p, const char* end, int depth, Ctx& c);
  bool parse_value(const char*& p, const char* end, int depth, Ctx& c, int32_t leaf, uint64_t path_hash);
  bool fail(Ctx& c, const char* at, const char* msg) {
    if (!c.err_at) {
      c.err_at = at;
      c.err_msg = msg;
    }
    return false;
  }

  ScanLimits limits_;
  std::string path_;
  std::string key_buf_;
};

}  // namespace zr::engine
