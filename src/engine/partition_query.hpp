// Ejecución de una consulta continua dentro de UNA partición (lado shard).
//
// Evalúa cada evento (WHERE, claves, argumentos de agregación) sobre los
// paneles abiertos y lleva la marca de agua de tiempo de evento:
//
//   marca = max_ts_visto + avance_por_inactividad - lateness
//
// Cuando la marca supera el final de un panel, el panel se cierra y se envía
// al coordinador, seguido de la propia marca (alineada a panel). Un evento cuyo
// panel ya está cerrado llega tarde y se descarta (y se cuenta).
//
// La consulta consume el log como un consumidor con cursor: si se creó con
// backfill, o tras un reinicio, está "poniéndose al día" leyendo del log hasta
// alcanzar el final y solo entonces pasa a recibir los lotes en vivo.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/arena.hpp"
#include "engine/aggregate.hpp"
#include "engine/field_layout.hpp"
#include "engine/pane.hpp"
#include "engine/types.hpp"
#include "storage/partition_log.hpp"

namespace zr::engine {

// Destino de lo que produce una consulta en una partición.
class QuerySink {
 public:
  virtual ~QuerySink() = default;
  virtual void pane_closed(uint64_t qid, uint32_t partition, std::unique_ptr<Pane> pane) = 0;
  virtual void watermark(uint64_t qid, uint32_t partition, int64_t wm) = 0;
  virtual void early_panes(uint64_t qid, uint32_t partition, std::vector<std::unique_ptr<Pane>> panes) = 0;
};

struct RuntimeOptions {
  int64_t idle_ms = 1000;
  int64_t skew_tolerance_ms = 60000;
  size_t max_groups = 100000;
};

class PartitionQuery {
 public:
  PartitionQuery(uint64_t qid, uint32_t partition, std::shared_ptr<const AggLayout> layout,
                 std::shared_ptr<QueryCounters> counters, int64_t start_ts, int64_t now_wall,
                 const RuntimeOptions& opts, QuerySink* sink);

  // Recalcula qué ranura de la partición corresponde a cada campo del plan.
  void bind(const FieldLayout& fields);

  void on_event(const EventView& ev, int64_t now_wall, Arena& scratch);
  // Avanza la marca de agua, cierra paneles y emite parciales si toca.
  void advance(int64_t now_wall);

  uint64_t id() const noexcept { return qid_; }
  uint32_t partition() const noexcept { return partition_; }
  int64_t start_ts() const noexcept { return start_ts_; }
  const zql::QueryPlan& plan() const noexcept { return layout_->plan(); }
  const std::shared_ptr<QueryCounters>& counters() const noexcept { return counters_; }
  size_t open_panes() const noexcept { return open_.size(); }

  // Estado de consumo del log.
  storage::LogCursor cursor;
  bool live = false;
  int64_t reported_lag = 0;  // último retraso sumado a counters()->lag

 private:
  Pane* pane_for(int64_t start);

  uint64_t qid_;
  uint32_t partition_;
  std::shared_ptr<const AggLayout> layout_;
  std::shared_ptr<QueryCounters> counters_;
  int64_t start_ts_;
  RuntimeOptions opts_;
  QuerySink* sink_;
  int64_t pane_ms_;

  std::vector<uint32_t> field_map_;
  std::map<int64_t, std::unique_ptr<Pane>> open_;
  Pane* last_pane_ = nullptr;
  std::string key_buf_;

  bool has_ts_ = false;
  int64_t max_ts_ = 0;
  int64_t last_event_wall_ = 0;
  int64_t created_wall_ = 0;
  bool closed_set_ = false;
  int64_t closed_upto_ = 0;
  int64_t last_early_wall_ = 0;
};

}  // namespace zr::engine
