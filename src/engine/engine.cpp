#include "engine/engine.hpp"

#include <algorithm>
#include <filesystem>
#include <thread>

#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "engine/catalog.hpp"
#include "json/writer.hpp"

namespace fs = std::filesystem;

namespace zr::engine {

Engine::Engine(EngineConfig cfg, EngineListener* listener) : cfg_(std::move(cfg)), listener_(listener) {}

Engine::~Engine() { stop(); }

bool Engine::start(std::string& err) {
  std::error_code ec;
  fs::create_directories(cfg_.data_dir, ec);
  if (ec) {
    err = cat("no se pudo crear el directorio de datos ", cfg_.data_dir, ": ", ec.message());
    return false;
  }
  uint32_t n = cfg_.shards;
  if (n == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    n = hw > 2 ? hw - 1 : 1;  // un núcleo para la red y el coordinador
  }
  cfg_.shards = n;
  coord_ = std::make_unique<Coordinator>(cfg_, listener_, &routing_);
  std::vector<Shard*> raw;
  for (uint32_t i = 0; i < n; ++i) {
    shards_.push_back(std::make_unique<Shard>(i, cfg_, coord_.get(), listener_));
    raw.push_back(shards_.back().get());
  }
  coord_->attach_shards(raw);
  for (auto& s : shards_) s->start();
  if (!coord_->load(err)) {
    for (auto& s : shards_) s->stop();
    return false;
  }
  coord_->start();
  started_ms_ = wall_ms();
  running_ = true;
  return true;
}

void Engine::stop() {
  if (!running_) return;
  running_ = false;
  // Primero los shards (vacían y sincronizan los logs), luego el coordinador.
  for (auto& s : shards_) s->stop();
  if (coord_) coord_->stop();
}

void Engine::ingest(std::string_view stream, std::string body, ReplyTo reply) {
  const auto routing = routing_.get();
  auto it = routing->streams.find(std::string(stream));
  msg::Ingest m;
  m.stream = std::string(stream);
  m.body = std::move(body);
  m.ingest_ms = wall_ms();
  m.reply = reply;
  if (it != routing->streams.end()) {
    const StreamRoute& r = *it->second;
    m.partition = r.next_partition->fetch_add(1, std::memory_order_relaxed) % r.cfg.partitions;
    shards_[r.shard_of[m.partition]]->post(std::move(m));
    return;
  }
  if (!cfg_.auto_create_streams) {
    if (listener_ && reply.valid())
      listener_->reply(reply, Response{404, "application/json",
                                       cat("{\"error\":\"no existe el stream '", stream, "'\"}")});
    return;
  }
  // Creación automática: el coordinador crea el stream y reenvía el lote.
  msg::CreateStream cs;
  cs.cfg.name = m.stream;
  cs.cfg.partitions = cfg_.default_partitions;
  cs.reply = reply;
  cs.if_missing = true;
  cs.then_ingest = std::move(m);
  coord_->post(std::move(cs));
}

void Engine::create_stream(StreamConfig cfg, ReplyTo reply) {
  msg::CreateStream m;
  m.cfg = std::move(cfg);
  m.reply = reply;
  coord_->post(std::move(m));
}

void Engine::delete_stream(std::string name, bool cascade, ReplyTo reply) {
  coord_->post(msg::DeleteStream{std::move(name), cascade, reply});
}

void Engine::create_query(std::string name, std::string zql, std::string from, ReplyTo reply) {
  coord_->post(msg::CreateQuery{std::move(name), std::move(zql), std::move(from), reply});
}

void Engine::delete_query(std::string name, ReplyTo reply) { coord_->post(msg::DeleteQuery{std::move(name), reply}); }

void Engine::get_query(std::string name, ReplyTo reply) { coord_->post(msg::GetQuery{std::move(name), reply}); }

void Engine::list_queries(ReplyTo reply) { coord_->post(msg::ListQueries{reply}); }

void Engine::get_results(std::string name, uint32_t last, ReplyTo reply) {
  coord_->post(msg::GetResults{std::move(name), last, reply});
}

void Engine::list_alerts(ReplyTo reply) { coord_->post(msg::ListAlerts{reply}); }

void Engine::metrics_history(ReplyTo reply) { coord_->post(msg::MetricsHistory{reply}); }

bool Engine::read_events(const std::string& stream, uint32_t partition, int64_t from, uint32_t limit, ReplyTo reply) {
  const auto routing = routing_.get();
  auto it = routing->streams.find(stream);
  if (it == routing->streams.end() || partition >= it->second->cfg.partitions) return false;
  shards_[it->second->shard_of[partition]]->post(msg::ReadEvents{stream, partition, from, limit, reply});
  return true;
}

bool Engine::add_tail(uint64_t tail_id, const std::string& stream, std::shared_ptr<const zql::FilterPlan> filter) {
  const auto routing = routing_.get();
  auto it = routing->streams.find(stream);
  if (it == routing->streams.end()) return false;
  std::vector<uint32_t> owners;
  for (uint32_t p = 0; p < it->second->cfg.partitions; ++p) {
    const uint32_t shard = it->second->shard_of[p];
    shards_[shard]->post(msg::AddTail{tail_id, stream, p, filter});
    if (std::find(owners.begin(), owners.end(), shard) == owners.end()) owners.push_back(shard);
  }
  tail_shards_[tail_id] = std::move(owners);
  return true;
}

void Engine::remove_tail(uint64_t tail_id) {
  auto it = tail_shards_.find(tail_id);
  if (it == tail_shards_.end()) return;
  for (uint32_t shard : it->second) shards_[shard]->post(msg::RemoveTail{tail_id});
  tail_shards_.erase(it);
}

namespace {

// Registros que conserva el log de una partición (los offsets son implícitos y
// consecutivos, así que basta restar; la retención mueve start_offset).
int64_t stored_records(const PartitionCounters& c) {
  return std::max<int64_t>(0, c.end_offset.load(std::memory_order_relaxed) - c.start_offset.load(std::memory_order_relaxed));
}

void write_route(json::Writer& w, const StreamRoute& r) {
  uint64_t events = 0, bytes = 0, rejected = 0, disk = 0;
  int64_t last = 0, stored = 0;
  for (const auto& c : r.counters) {
    events += c->events.load(std::memory_order_relaxed);
    bytes += c->bytes.load(std::memory_order_relaxed);
    rejected += c->rejected.load(std::memory_order_relaxed);
    disk += c->disk_bytes.load(std::memory_order_relaxed);
    last = std::max(last, c->last_event_ms.load(std::memory_order_relaxed));
    stored += stored_records(*c);
  }
  w.begin_object()
      .kv("name", r.cfg.name)
      .kv_int("partitions", r.cfg.partitions)
      .kv("ts_field", r.cfg.ts_field)
      .kv_int("created_ms", r.cfg.created_ms)
      .kv_int("events", static_cast<int64_t>(events))
      .kv_int("stored", stored)
      .kv_int("bytes_in", static_cast<int64_t>(bytes))
      .kv_int("rejected", static_cast<int64_t>(rejected))
      .kv_int("disk_bytes", static_cast<int64_t>(disk))
      .kv_int("last_event_ms", last);
  w.key("partition_stats").begin_array();
  for (size_t p = 0; p < r.counters.size(); ++p) {
    const auto& c = *r.counters[p];
    w.begin_object()
        .kv_int("partition", static_cast<int64_t>(p))
        .kv_int("shard", r.shard_of[p])
        .kv_int("start_offset", c.start_offset.load())
        .kv_int("end_offset", c.end_offset.load())
        .kv_int("segments", c.segments.load())
        .kv_int("disk_bytes", static_cast<int64_t>(c.disk_bytes.load()))
        .end_object();
  }
  w.end_array();
  w.key("queries").begin_array();
  for (const auto& q : r.queries) w.str(q);
  w.end_array().end_object();
}

}  // namespace

std::string Engine::streams_json() const {
  const auto routing = routing_.get();
  std::string out;
  json::Writer w(out);
  w.begin_array();
  for (const auto& [name, r] : routing->streams) write_route(w, *r);
  w.end_array();
  return out;
}

std::string Engine::stream_json(const std::string& name, bool& found) const {
  const auto routing = routing_.get();
  auto it = routing->streams.find(name);
  found = it != routing->streams.end();
  if (!found) return {};
  std::string out;
  json::Writer w(out);
  write_route(w, *it->second);
  return out;
}

std::string Engine::stats_json() const {
  const auto routing = routing_.get();
  uint64_t events = 0, disk = 0;
  int64_t stored = 0;
  for (const auto& [name, r] : routing->streams)
    for (const auto& c : r->counters) {
      events += c->events.load(std::memory_order_relaxed);
      disk += c->disk_bytes.load(std::memory_order_relaxed);
      stored += stored_records(*c);
    }
  std::string out;
  json::Writer w(out);
  w.begin_object()
      .kv("version", ZENRIFT_VERSION)
      .kv_int("started_ms", started_ms_)
      .kv_int("uptime_ms", wall_ms() - started_ms_)
      .kv_int("shards", static_cast<int64_t>(shards_.size()))
      .kv_int("streams", static_cast<int64_t>(routing->streams.size()))
      .kv_int("events_total", static_cast<int64_t>(events))
      .kv_int("events_stored", stored)
      .kv_int("disk_bytes", static_cast<int64_t>(disk))
      .kv("data_dir", cfg_.data_dir)
      .kv("fsync", cfg_.fsync == FsyncPolicy::Always ? "always" : cfg_.fsync == FsyncPolicy::Interval ? "interval" : "never");
  w.key("queues").begin_object().kv_int("coordinator", static_cast<int64_t>(coord_ ? coord_->queue_depth() : 0));
  w.key("shards").begin_array();
  for (const auto& s : shards_) w.integer(static_cast<int64_t>(s->queue_depth()));
  w.end_array().end_object().end_object();
  return out;
}

}  // namespace zr::engine
