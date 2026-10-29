// Coordinador: dueño del catálogo (streams y consultas), de la finalización de
// ventanas, de las alertas y de las métricas globales.
//
// Recibe peticiones de la API y los paneles/marcas que emiten los shards;
// publica resultados, alertas y métricas a través del EngineListener. Todo su
// estado es suyo: ningún otro hilo lo toca.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/mpsc_queue.hpp"
#include "engine/alerts.hpp"
#include "engine/catalog.hpp"
#include "engine/finalizer.hpp"
#include "engine/messages.hpp"
#include "engine/routing.hpp"
#include "engine/shard.hpp"

namespace zr::engine {

class Coordinator final : public CoordinatorPort {
 public:
  Coordinator(const EngineConfig& cfg, EngineListener* listener, RoutingTable* routing);
  ~Coordinator() override;

  void attach_shards(std::vector<Shard*> shards);
  // Carga el catálogo y prepara particiones y consultas (antes de start()).
  bool load(std::string& err);
  void start();
  void stop();
  void post(msg::CoordMsg m) override;

  size_t queue_depth() const { return inbox_.approx_size(); }

 private:
  struct StreamState {
    StreamConfig cfg;
    std::vector<uint32_t> shard_of;
    std::vector<std::shared_ptr<PartitionCounters>> counters;
    std::shared_ptr<std::atomic<uint32_t>> next_partition;
    std::vector<std::string> queries;
  };

  struct QueryState {
    QueryDef def;
    std::shared_ptr<const zql::QueryPlan> plan;
    std::shared_ptr<const AggLayout> layout;
    std::unique_ptr<QueryFinalizer> fin;
    std::shared_ptr<QueryCounters> counters;
  };

  void run();
  void handle(msg::CoordMsg& m);
  void on(msg::PaneClosed& m);
  void on(msg::Watermark& m);
  void on(msg::EarlyPanes& m);
  void on(msg::QueryStarted& m);
  void on(msg::CreateStream& m);
  void on(msg::DeleteStream& m);
  void on(msg::CreateQuery& m);
  void on(msg::DeleteQuery& m);
  void on(msg::GetQuery& m);
  void on(msg::ListQueries& m);
  void on(msg::GetResults& m);
  void on(msg::ListAlerts& m);
  void on(msg::MetricsHistory& m);

  StreamState* create_stream(StreamConfig cfg);
  bool start_query(QueryDef def, bool recovering, Response& error);
  void stop_query(const std::string& name);
  void forward_ingest(StreamState& s, msg::Ingest&& ingest);
  void dispatch(QueryState& q, FinalizerOutput& out);
  void publish_routing();
  void publish_catalog();
  void persist();
  void tick(int64_t now);
  std::string query_json(const QueryState& q, bool detailed) const;
  void reply(const ReplyTo& to, int status, std::string body);
  void reply_error(const ReplyTo& to, int status, std::string_view message);

  EngineConfig cfg_;
  EngineListener* listener_;
  RoutingTable* routing_;
  std::vector<Shard*> shards_;
  MpscQueue<msg::CoordMsg> inbox_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::map<std::string, StreamState> streams_;
  std::map<std::string, std::unique_ptr<QueryState>> queries_;
  std::unordered_map<uint64_t, QueryState*> by_id_;
  std::vector<QueryDef> broken_;  // del catálogo, sin compilar: se conservan
  AlertBook alerts_;
  uint64_t next_qid_ = 1;
  int64_t started_ms_ = 0;

  // Métricas.
  int64_t last_tick_ = 0;
  uint64_t last_events_ = 0;
  uint64_t last_bytes_ = 0;
  uint64_t last_late_ = 0;
  uint64_t windows_since_tick_ = 0;
  std::deque<std::shared_ptr<const std::string>> metrics_;
};

}  // namespace zr::engine
