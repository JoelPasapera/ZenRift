// Mensajes entre hilos. Cada partición tiene un único dueño (un shard) y el
// catálogo y la finalización tienen otro (el coordinador): nadie comparte
// estado mutable, todo cruza hilos como mensaje por una MpscQueue.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "engine/aggregate.hpp"
#include "engine/pane.hpp"
#include "engine/types.hpp"
#include "zql/plan.hpp"

namespace zr::engine::msg {

// ----------------------------------------------------------------- al shard

struct OpenPartition {
  StreamConfig cfg;
  uint32_t partition = 0;
  std::shared_ptr<PartitionCounters> counters;
};

struct ClosePartition {
  std::string stream;
  uint32_t partition = 0;
  bool destroy = false;
};

struct Ingest {
  std::string stream;
  uint32_t partition = 0;
  std::string body;
  int64_t ingest_ms = 0;
  ReplyTo reply;
};

struct AddQuery {
  std::string stream;
  uint32_t partition = 0;
  uint64_t qid = 0;
  std::shared_ptr<const AggLayout> layout;
  std::shared_ptr<QueryCounters> counters;
  int64_t start_ts = 0;
  bool backfill = false;     // leer el log desde start_ts en vez de empezar en vivo
  int64_t start_offset = -1; // con backfill: leer desde este offset en vez de por tiempo
  int64_t horizon_ts = 0;    // con start_offset: no antes del segmento que contiene este instante
};

struct RemoveQuery {
  uint64_t qid = 0;
};

struct AddTail {
  uint64_t tail_id = 0;
  std::string stream;
  uint32_t partition = 0;
  std::shared_ptr<const zql::FilterPlan> filter;  // nullptr = todos los eventos
};

struct RemoveTail {
  uint64_t tail_id = 0;
};

struct ReadEvents {
  std::string stream;
  uint32_t partition = 0;
  int64_t from = 0;
  uint32_t limit = 100;
  ReplyTo reply;
};

using ShardMsg = std::variant<OpenPartition, ClosePartition, Ingest, AddQuery, RemoveQuery, AddTail, RemoveTail,
                              ReadEvents>;

// ----------------------------------------------------------- al coordinador

struct PaneClosed {
  uint64_t qid = 0;
  uint32_t partition = 0;
  std::unique_ptr<Pane> pane;
};

struct Watermark {
  uint64_t qid = 0;
  uint32_t partition = 0;
  int64_t wm = 0;
};

struct EarlyPanes {
  uint64_t qid = 0;
  uint32_t partition = 0;
  std::vector<std::unique_ptr<Pane>> panes;
};

// Una consulta "now" empezó en vivo en una partición: offset desde el que cuenta.
struct QueryStarted {
  uint64_t qid = 0;
  uint32_t partition = 0;
  int64_t offset = 0;
};

struct CreateStream {
  StreamConfig cfg;
  ReplyTo reply;
  bool if_missing = false;              // no es error si ya existe
  std::optional<Ingest> then_ingest;    // creación automática al recibir eventos
};

struct DeleteStream {
  std::string name;
  bool cascade = false;
  ReplyTo reply;
};

struct CreateQuery {
  std::string name;
  std::string zql;
  std::string from;
  ReplyTo reply;
};

struct DeleteQuery {
  std::string name;
  ReplyTo reply;
};

struct GetQuery {
  std::string name;
  ReplyTo reply;
};

struct ListQueries {
  ReplyTo reply;
};

struct GetResults {
  std::string name;
  uint32_t last = 60;
  ReplyTo reply;
};

struct ListAlerts {
  ReplyTo reply;
};

struct MetricsHistory {
  ReplyTo reply;
};

using CoordMsg = std::variant<PaneClosed, Watermark, EarlyPanes, QueryStarted, CreateStream, DeleteStream, CreateQuery,
                              DeleteQuery, GetQuery, ListQueries, GetResults, ListAlerts, MetricsHistory>;

}  // namespace zr::engine::msg
