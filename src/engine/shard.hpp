// Shard: un hilo dueño de un conjunto de particiones.
//
// Para cada partición guarda el log en disco, la disposición de campos que
// necesitan sus consultas, las consultas en ejecución y los tails en vivo.
// Procesa lotes de ingesta (escanear -> anexar al log -> evaluar), pone al día
// las consultas con backfill leyendo el log, y cada 100 ms avanza marcas de
// agua, hace fsync según la política y aplica la retención.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/arena.hpp"
#include "core/mpsc_queue.hpp"
#include "engine/event_scanner.hpp"
#include "engine/field_layout.hpp"
#include "engine/messages.hpp"
#include "engine/partition_query.hpp"
#include "storage/partition_log.hpp"

namespace zr::engine {

// Lo que un shard necesita del coordinador.
class CoordinatorPort {
 public:
  virtual ~CoordinatorPort() = default;
  virtual void post(msg::CoordMsg m) = 0;
};

class Shard final : public QuerySink {
 public:
  Shard(uint32_t id, const EngineConfig& cfg, CoordinatorPort* coord, EngineListener* listener);
  ~Shard() override;

  void start();
  void stop();
  void post(msg::ShardMsg m);
  size_t queue_depth() const { return inbox_.approx_size(); }

  // QuerySink: lo producido por las consultas va al coordinador.
  void pane_closed(uint64_t qid, uint32_t partition, std::unique_ptr<Pane> pane) override;
  void watermark(uint64_t qid, uint32_t partition, int64_t wm) override;
  void early_panes(uint64_t qid, uint32_t partition, std::vector<std::unique_ptr<Pane>> panes) override;

 private:
  struct Tail {
    uint64_t id = 0;
    std::shared_ptr<const zql::FilterPlan> filter;
    std::vector<uint32_t> field_map;
    double budget = 0;
    int64_t last_refill = 0;
    uint64_t dropped = 0;
  };

  struct Partition {
    StreamConfig cfg;
    uint32_t index = 0;
    std::shared_ptr<PartitionCounters> counters;
    storage::PartitionLog log;
    FieldLayout layout;
    std::vector<std::unique_ptr<PartitionQuery>> queries;
    std::vector<Tail> tails;
    bool unsynced = false;
    int64_t last_sync = 0;
  };

  using Key = std::pair<std::string, uint32_t>;

  void run();
  void handle(msg::ShardMsg& m, int64_t now);
  void on(msg::OpenPartition& m, int64_t now);
  void on(msg::ClosePartition& m, int64_t now);
  void on(msg::Ingest& m, int64_t now);
  void on(msg::AddQuery& m, int64_t now);
  void on(msg::RemoveQuery& m, int64_t now);
  void on(msg::AddTail& m, int64_t now);
  void on(msg::RemoveTail& m, int64_t now);
  void on(msg::ReadEvents& m, int64_t now);

  Partition* find(const std::string& stream, uint32_t partition);
  void rebuild_layout(Partition& p);
  void feed_tails(Partition& p, const std::vector<ScannedEvent>& events, const std::vector<zql::Value>& slots,
                  int64_t now);
  bool catch_up(int64_t now);  // true si queda trabajo pendiente
  void tick(int64_t now);
  void update_counters(Partition& p);
  void reply(const ReplyTo& to, int status, std::string body);

  uint32_t id_;
  EngineConfig cfg_;
  CoordinatorPort* coord_;
  EngineListener* listener_;
  RuntimeOptions ropts_;
  MpscQueue<msg::ShardMsg> inbox_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::map<Key, std::unique_ptr<Partition>> parts_;
  EventScanner scanner_;
  Arena arena_;
  std::vector<ScannedEvent> events_;
  std::vector<zql::Value> slots_;
  std::string read_buf_;
  std::vector<storage::LogRecord> records_;
  int64_t last_tick_ = 0;
  int64_t last_retention_ = 0;
};

}  // namespace zr::engine
