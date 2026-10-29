#include "engine/shard.hpp"

#include <algorithm>
#include <filesystem>

#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "json/writer.hpp"
#include "zql/vm.hpp"

namespace fs = std::filesystem;

namespace zr::engine {

namespace {
constexpr int64_t kTickMs = 100;
constexpr int64_t kRetentionEveryMs = 30000;
constexpr size_t kCatchUpBatch = 2048;
}  // namespace

Shard::Shard(uint32_t id, const EngineConfig& cfg, CoordinatorPort* coord, EngineListener* listener)
    : id_(id), cfg_(cfg), coord_(coord), listener_(listener), arena_(256 * 1024) {
  ropts_.idle_ms = cfg.idle_ms;
  ropts_.max_groups = cfg.max_groups;
  ScanLimits limits;
  limits.max_event_bytes = cfg.max_event_bytes;
  limits.max_future_ms = cfg.max_future_ms;
  scanner_ = EventScanner(limits);
}

Shard::~Shard() { stop(); }

void Shard::start() {
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void Shard::stop() {
  if (!thread_.joinable()) return;
  running_ = false;
  inbox_.wake();
  thread_.join();
}

void Shard::post(msg::ShardMsg m) { inbox_.push(std::move(m)); }

void Shard::pane_closed(uint64_t qid, uint32_t partition, std::unique_ptr<Pane> pane) {
  coord_->post(msg::PaneClosed{qid, partition, std::move(pane)});
}

void Shard::watermark(uint64_t qid, uint32_t partition, int64_t wm) {
  coord_->post(msg::Watermark{qid, partition, wm});
}

void Shard::early_panes(uint64_t qid, uint32_t partition, std::vector<std::unique_ptr<Pane>> panes) {
  coord_->post(msg::EarlyPanes{qid, partition, std::move(panes)});
}

void Shard::reply(const ReplyTo& to, int status, std::string body) {
  if (!to.valid() || !listener_) return;
  Response r;
  r.status = status;
  r.body = std::move(body);
  listener_->reply(to, std::move(r));
}

void Shard::run() {
  log::set_thread_name(cat("shard-", id_));
  std::vector<msg::ShardMsg> batch;
  bool pending = false;
  while (running_) {
    const auto deadline = pending ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::now() + std::chrono::milliseconds(kTickMs);
    inbox_.wait_pop_all(batch, deadline);
    for (auto& m : batch) handle(m, wall_ms());
    batch.clear();
    const int64_t now = wall_ms();
    if (now - last_tick_ >= kTickMs) tick(now);
    pending = catch_up(now);
  }
  // Cierre ordenado: todo lo aceptado queda en disco.
  for (auto& [key, p] : parts_) {
    std::string err;
    if (!p->log.sync(err)) log::error(err);
  }
  parts_.clear();
}

void Shard::handle(msg::ShardMsg& m, int64_t now) {
  std::visit([&](auto& x) { on(x, now); }, m);
}

Shard::Partition* Shard::find(const std::string& stream, uint32_t partition) {
  auto it = parts_.find(Key{stream, partition});
  return it == parts_.end() ? nullptr : it->second.get();
}

void Shard::on(msg::OpenPartition& m, int64_t now) {
  const Key key{m.cfg.name, m.partition};
  if (parts_.count(key)) return;
  auto p = std::make_unique<Partition>();
  p->cfg = m.cfg;
  p->index = m.partition;
  p->counters = m.counters;
  storage::LogOptions opts;
  opts.segment_bytes = cfg_.segment_bytes;
  opts.max_record_bytes = static_cast<uint32_t>(std::max<size_t>(cfg_.max_event_bytes, 1024));
  const std::string dir = (fs::path(cfg_.data_dir) / "streams" / m.cfg.name / cat("p", m.partition)).string();
  std::string err;
  if (!p->log.open(dir, opts, err)) {
    log::error("no se pudo abrir la partición ", m.cfg.name, "/", m.partition, ": ", err);
    return;
  }
  for (const auto& w : p->log.recovery().warnings) log::warn("recuperación de ", m.cfg.name, "/", m.partition, ": ", w);
  if (p->log.recovery().records > 0)
    log::info("partición ", m.cfg.name, "/", m.partition, " abierta: ", p->log.recovery().records, " eventos en ",
              p->log.segment_count(), " segmento(s)");
  p->last_sync = now;
  rebuild_layout(*p);
  update_counters(*p);
  p->counters->open = true;
  parts_.emplace(key, std::move(p));
}

void Shard::on(msg::ClosePartition& m, int64_t) {
  auto it = parts_.find(Key{m.stream, m.partition});
  if (it == parts_.end()) return;
  std::string err;
  if (m.destroy) {
    if (!it->second->log.destroy(err)) log::warn(err);
    // El directorio del stream se borra cuando queda vacío: lo consigue el
    // shard que destruye la última partición (remove no borra si hay algo).
    std::error_code ec;
    fs::remove(fs::path(it->second->log.dir()).parent_path(), ec);
  } else if (!it->second->log.sync(err)) {
    log::error(err);
  }
  it->second->counters->open = false;
  parts_.erase(it);
}

void Shard::rebuild_layout(Partition& p) {
  std::vector<std::string> paths{p.cfg.ts_field};
  auto add = [&](const std::string& f) {
    if (std::find(paths.begin(), paths.end(), f) == paths.end()) paths.push_back(f);
  };
  for (const auto& q : p.queries)
    for (const auto& f : q->plan().fields) add(f);
  for (const auto& t : p.tails)
    if (t.filter)
      for (const auto& f : t.filter->fields) add(f);
  p.layout.build(std::move(paths));
  for (auto& q : p.queries) q->bind(p.layout);
  for (auto& t : p.tails) {
    t.field_map.clear();
    if (t.filter)
      for (const auto& f : t.filter->fields) t.field_map.push_back(static_cast<uint32_t>(p.layout.slot_of(f)));
  }
}

void Shard::update_counters(Partition& p) {
  p.counters->start_offset = p.log.start_offset();
  p.counters->end_offset = p.log.end_offset();
  p.counters->disk_bytes = p.log.size_bytes();
  p.counters->segments = static_cast<uint32_t>(p.log.segment_count());
}

void Shard::on(msg::Ingest& m, int64_t now) {
  Partition* p = find(m.stream, m.partition);
  if (!p) {
    reply(m.reply, 503, cat("{\"error\":\"la partición ", m.stream, "/", m.partition, " no está abierta\"}"));
    return;
  }
  arena_.reset();
  ScanReport report;
  scanner_.scan_batch(m.body, p->layout, 0, m.ingest_ms, arena_, events_, slots_, report);

  const int64_t first = p->log.end_offset();
  std::string err;
  size_t appended = 0;
  int status = 200;
  for (const auto& ev : events_) {
    int64_t off;
    if (!p->log.append(ev.ts, ev.raw, off, err)) {
      status = 507;
      break;
    }
    ++appended;
  }
  if (appended > 0) {
    if (!p->log.flush(err)) status = 507;
    if (cfg_.fsync == FsyncPolicy::Always && !p->log.sync(err)) status = 507;
    p->unsynced = cfg_.fsync == FsyncPolicy::Interval;
  }
  if (appended < events_.size()) events_.resize(appended);

  p->counters->events.fetch_add(appended, std::memory_order_relaxed);
  p->counters->bytes.fetch_add(m.body.size(), std::memory_order_relaxed);
  p->counters->rejected.fetch_add(report.rejected, std::memory_order_relaxed);
  if (appended) p->counters->last_event_ms = now;
  p->counters->end_offset = p->log.end_offset();

  // Consultas en vivo: evalúan el lote directamente desde memoria.
  const size_t L = p->layout.size();
  for (auto& q : p->queries) {
    if (!q->live || q->cursor.offset != first) continue;
    for (size_t i = 0; i < events_.size(); ++i)
      q->on_event(EventView{slots_.data() + i * L, events_[i].ts, events_[i].raw}, now, arena_);
    q->cursor.offset = first + static_cast<int64_t>(events_.size());
    q->cursor.segment_base = -1;
    q->advance(now);
  }
  if (!p->tails.empty() && !events_.empty()) feed_tails(*p, events_, slots_, now);

  if (!m.reply.valid()) return;
  std::string body;
  json::Writer w(body);
  w.begin_object()
      .kv("stream", m.stream)
      .kv_int("partition", m.partition)
      .kv_int("accepted", static_cast<int64_t>(appended))
      .kv_int("rejected", static_cast<int64_t>(report.rejected));
  if (appended) w.kv_int("first_offset", first).kv_int("last_offset", first + static_cast<int64_t>(appended) - 1);
  if (status == 507) w.kv("error", cat("no se pudo escribir en disco: ", err));
  w.key("errors").begin_array();
  for (const auto& e : report.errors) w.str(e);
  w.end_array().end_object();
  if (status == 200 && appended == 0 && report.rejected > 0) status = 400;
  reply(m.reply, status, std::move(body));
}

void Shard::feed_tails(Partition& p, const std::vector<ScannedEvent>& events, const std::vector<zql::Value>& slots,
                       int64_t now) {
  const size_t L = p.layout.size();
  const double rate = cfg_.tail_max_events_per_sec;
  for (auto& t : p.tails) {
    t.budget = std::min(rate, t.budget + rate * static_cast<double>(now - t.last_refill) / 1000.0);
    t.last_refill = now;
    std::string out;
    json::Writer w(out);
    w.begin_array();
    size_t sent = 0;
    for (size_t i = 0; i < events.size(); ++i) {
      if (t.filter) {
        zql::EvalContext ctx;
        ctx.fields = slots.data() + i * L;
        ctx.field_map = t.field_map.data();
        ctx.scratch = &arena_;
        if (!zql::eval_true(t.filter->prog, ctx)) continue;
      }
      if (t.budget < 1) {
        ++t.dropped;
        continue;
      }
      t.budget -= 1;
      w.raw(events[i].raw);
      ++sent;
    }
    w.end_array();
    if (sent) listener_->publish(Publication{Topic::Tail, std::to_string(t.id), "events",
                                             std::make_shared<const std::string>(std::move(out))});
    if (t.dropped && t.budget >= 1) {
      listener_->publish(Publication{Topic::Tail, std::to_string(t.id), "dropped",
                                     std::make_shared<const std::string>(cat("{\"dropped\":", t.dropped, "}"))});
      t.dropped = 0;
    }
  }
}

void Shard::on(msg::AddQuery& m, int64_t now) {
  Partition* p = find(m.stream, m.partition);
  if (!p) {
    log::warn("consulta ", m.qid, " sobre una partición no abierta: ", m.stream, "/", m.partition);
    return;
  }
  auto q = std::make_unique<PartitionQuery>(m.qid, m.partition, m.layout, m.counters, m.start_ts, now, ropts_, this);
  if (m.backfill && m.start_offset >= 0) {
    // Reanudación de una consulta "now": desde su offset de creación, sin
    // retroceder más allá del horizonte de recuperación ni de la retención.
    q->cursor.offset = std::max({m.start_offset, p->log.start_offset(), p->log.offset_for_time(m.horizon_ts)});
    q->cursor.offset = std::min(q->cursor.offset, p->log.end_offset());
    q->live = false;
  } else if (m.backfill) {
    q->cursor.offset = p->log.offset_for_time(m.start_ts);
    q->live = false;
  } else {
    q->cursor.offset = p->log.end_offset();
    q->live = true;
    m.counters->partitions_live.fetch_add(1, std::memory_order_relaxed);
    coord_->post(msg::QueryStarted{m.qid, m.partition, q->cursor.offset});
  }
  p->queries.push_back(std::move(q));
  rebuild_layout(*p);
}

void Shard::on(msg::RemoveQuery& m, int64_t) {
  for (auto& [key, p] : parts_) {
    const auto before = p->queries.size();
    p->queries.erase(std::remove_if(p->queries.begin(), p->queries.end(),
                                    [&](const std::unique_ptr<PartitionQuery>& q) { return q->id() == m.qid; }),
                     p->queries.end());
    if (p->queries.size() != before) rebuild_layout(*p);
  }
}

void Shard::on(msg::AddTail& m, int64_t now) {
  Partition* p = find(m.stream, m.partition);
  if (!p) return;
  Tail t;
  t.id = m.tail_id;
  t.filter = m.filter;
  t.budget = cfg_.tail_max_events_per_sec;
  t.last_refill = now;
  p->tails.push_back(std::move(t));
  rebuild_layout(*p);
}

void Shard::on(msg::RemoveTail& m, int64_t) {
  for (auto& [key, p] : parts_) {
    const auto before = p->tails.size();
    p->tails.erase(std::remove_if(p->tails.begin(), p->tails.end(), [&](const Tail& t) { return t.id == m.tail_id; }),
                   p->tails.end());
    if (p->tails.size() != before) rebuild_layout(*p);
  }
}

void Shard::on(msg::ReadEvents& m, int64_t) {
  Partition* p = find(m.stream, m.partition);
  if (!p) {
    reply(m.reply, 404, cat("{\"error\":\"no existe la partición ", m.stream, "/", m.partition, "\"}"));
    return;
  }
  storage::LogCursor c;
  c.offset = std::max<int64_t>(m.from, 0);
  std::string err;
  if (!p->log.read(c, std::clamp<uint32_t>(m.limit, 1, 10000), 32u << 20, read_buf_, records_, err)) {
    reply(m.reply, 500, cat("{\"error\":\"", err, "\"}"));
    return;
  }
  std::string body;
  json::Writer w(body);
  w.begin_object()
      .kv("stream", m.stream)
      .kv_int("partition", m.partition)
      .kv_int("from", records_.empty() ? c.offset : records_.front().offset)
      .kv_int("next", c.offset)
      .kv_int("start", p->log.start_offset())
      .kv_int("end", p->log.end_offset());
  w.key("events").begin_array();
  for (const auto& r : records_) {
    w.begin_object().kv_int("offset", r.offset).kv_int("ts", r.ts).key("event").raw(r.payload).end_object();
  }
  w.end_array().end_object();
  reply(m.reply, 200, std::move(body));
}

bool Shard::catch_up(int64_t now) {
  bool more = false;
  std::vector<zql::Value> slots;
  for (auto& [key, p] : parts_) {
    for (auto& q : p->queries) {
      if (q->live) continue;
      std::string err;
      if (!p->log.read(q->cursor, kCatchUpBatch, 8u << 20, read_buf_, records_, err)) {
        log::error("backfill de la consulta ", q->id(), " en ", p->cfg.name, "/", p->index, ": ", err);
        q->cursor.offset = p->log.end_offset();
        q->cursor.segment_base = -1;
      }
      arena_.reset();
      slots.resize(p->layout.size());
      for (const auto& r : records_) {
        if (scanner_.scan_stored(r.payload, p->layout, arena_, slots.data()))
          q->on_event(EventView{slots.data(), r.ts, r.payload}, now, arena_);
      }
      q->advance(now);
      const int64_t lag = std::max<int64_t>(0, p->log.end_offset() - q->cursor.offset);
      q->counters()->lag.fetch_add(lag - q->reported_lag, std::memory_order_relaxed);
      q->reported_lag = lag;
      if (lag == 0) {
        q->live = true;
        q->counters()->partitions_live.fetch_add(1, std::memory_order_relaxed);
      } else {
        more = true;
      }
    }
  }
  return more;
}

void Shard::tick(int64_t now) {
  last_tick_ = now;
  const bool retention = now - last_retention_ >= kRetentionEveryMs;
  if (retention) last_retention_ = now;
  for (auto& [key, p] : parts_) {
    for (auto& q : p->queries)
      if (q->live) q->advance(now);
    if (p->unsynced && now - p->last_sync >= cfg_.fsync_interval_ms) {
      std::string err;
      if (!p->log.sync(err)) log::error(err);
      p->unsynced = false;
      p->last_sync = now;
    }
    if (retention) {
      const uint64_t max_bytes = p->cfg.retention_bytes ? p->cfg.retention_bytes : cfg_.default_retention_bytes;
      const int64_t max_age = p->cfg.retention_ms ? p->cfg.retention_ms : cfg_.default_retention_ms;
      const size_t removed = p->log.enforce_retention(now, max_bytes, max_age);
      if (removed) log::info("retención en ", p->cfg.name, "/", p->index, ": ", removed, " segmento(s) borrados");
    }
    update_counters(*p);
  }
}

}  // namespace zr::engine
