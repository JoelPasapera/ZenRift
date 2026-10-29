// Tipos compartidos del motor: configuración, catálogo, contadores, la vista
// de un evento y la interfaz con la capa de red.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zql/plan.hpp"
#include "zql/value.hpp"

namespace zr::engine {

// Evento listo para evaluar: campos extraídos según la FieldLayout de la partición.
struct EventView {
  const zql::Value* slots = nullptr;
  int64_t ts = 0;
  std::string_view raw;
};

enum class FsyncPolicy { Always, Interval, Never };

struct EngineConfig {
  std::string data_dir = "./data";
  uint32_t shards = 0;  // 0 = automático
  FsyncPolicy fsync = FsyncPolicy::Interval;
  int64_t fsync_interval_ms = 1000;
  uint64_t segment_bytes = 64ull << 20;
  uint64_t default_retention_bytes = 1ull << 30;  // por partición
  int64_t default_retention_ms = 7LL * 86400000;
  uint32_t default_partitions = 1;
  size_t max_event_bytes = 1u << 20;
  int64_t max_future_ms = 3600000;
  int64_t idle_ms = 1000;                 // sin eventos durante esto, el tiempo de evento avanza con el reloj
  size_t max_groups = 100000;             // por panel y consulta
  size_t max_rows = 1000;                 // filas por resultado si no hay LIMIT
  size_t history_windows = 360;           // resultados finales que se guardan por consulta
  int64_t recovery_horizon_ms = 3600000;  // al arrancar, las consultas re-procesan esta historia
  bool auto_create_streams = true;
  uint32_t tail_max_events_per_sec = 500;
  zql::CompileOptions compile;
};

struct StreamConfig {
  std::string name;
  uint32_t partitions = 1;
  std::string ts_field = "ts";
  uint64_t retention_bytes = 0;
  int64_t retention_ms = 0;
  int64_t created_ms = 0;
};

struct QueryDef {
  uint64_t id = 0;
  std::string name;
  std::string zql;
  std::string from;     // "now", "beginning", "1h" o RFC 3339, tal como se pidió
  int64_t start_ms = 0; // instante resuelto desde el que cuenta la consulta
  int64_t created_ms = 0;
  // Consultas "now": offset del log de cada partición cuando se creó (-1 si
  // aún no se conoce). Al reiniciar se reanudan desde ahí: cuentan lo que
  // llegó desde su creación, aunque traiga marcas de tiempo antiguas.
  std::vector<int64_t> start_offsets;
};

// Contadores por partición, leídos sin bloqueo por las estadísticas.
struct PartitionCounters {
  std::atomic<uint64_t> events{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> rejected{0};
  std::atomic<int64_t> start_offset{0};
  std::atomic<int64_t> end_offset{0};
  std::atomic<uint64_t> disk_bytes{0};
  std::atomic<uint32_t> segments{0};
  std::atomic<int64_t> last_event_ms{0};
  std::atomic<bool> open{false};
};

// Contadores de una consulta (sumados entre particiones).
struct QueryCounters {
  std::atomic<uint64_t> events{0};    // eventos vistos
  std::atomic<uint64_t> matched{0};   // pasaron el WHERE
  std::atomic<uint64_t> late{0};      // llegaron tarde y se descartaron
  std::atomic<uint64_t> dropped{0};   // descartados por el límite de grupos
  std::atomic<int64_t> lag{0};        // registros pendientes de procesar (backfill)
  std::atomic<uint32_t> partitions_live{0};
  std::atomic<uint64_t> windows{0};
  std::atomic<uint64_t> partials{0};
  std::atomic<int64_t> last_window_end{0};
};

// Destino de una respuesta asíncrona: lo interpreta la capa de red.
struct ReplyTo {
  uint64_t conn = 0;
  uint64_t request = 0;
  bool valid() const noexcept { return conn != 0; }
};

struct Response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

enum class Topic : uint8_t {
  QueryResult,   // key = nombre de la consulta
  QueryPartial,  // key = nombre de la consulta
  Alert,         // key vacía
  Tail,          // key = id del tail
  Metrics,       // key vacía
  Catalog,       // key vacía: cambiaron streams o consultas
};

struct Publication {
  Topic topic;
  std::string key;
  std::string event;  // nombre del evento SSE
  std::shared_ptr<const std::string> data;
};

// Implementada por la capa de red. Se llama desde los hilos del motor: debe
// ser segura entre hilos y no bloquear.
class EngineListener {
 public:
  virtual ~EngineListener() = default;
  virtual void reply(const ReplyTo& to, Response response) = 0;
  virtual void publish(Publication p) = 0;
};

bool valid_name(std::string_view name) noexcept;

}  // namespace zr::engine
