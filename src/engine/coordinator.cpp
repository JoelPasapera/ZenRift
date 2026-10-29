#include "engine/coordinator.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "json/writer.hpp"

namespace fs = std::filesystem;

namespace zr::engine {

namespace {

constexpr int64_t kMetricsEveryMs = 1000;
constexpr size_t kMetricsHistory = 300;

std::string error_json(std::string_view message) {
  std::string out;
  json::Writer(out).begin_object().kv("error", message).end_object();
  return out;
}

// "now", "beginning", "1h" (hacia atrás) o RFC 3339.
bool counts_from_now(std::string_view from) { return from.empty() || from == "now" || from == "ahora"; }

bool resolve_start(std::string_view from, int64_t now, int64_t& start, bool& backfill) {
  if (counts_from_now(from)) {
    start = now;
    backfill = false;
    return true;
  }
  if (from == "beginning" || from == "earliest" || from == "inicio") {
    start = 0;
    backfill = true;
    return true;
  }
  int64_t d;
  if (parse_duration(from, d)) {
    start = now - d;
    backfill = true;
    return true;
  }
  if (parse_rfc3339(from, start)) {
    backfill = true;
    return true;
  }
  return false;
}

}  // namespace

Coordinator::Coordinator(const EngineConfig& cfg, EngineListener* listener, RoutingTable* routing)
    : cfg_(cfg), listener_(listener), routing_(routing) {}

Coordinator::~Coordinator() { stop(); }

void Coordinator::attach_shards(std::vector<Shard*> shards) { shards_ = std::move(shards); }

void Coordinator::post(msg::CoordMsg m) { inbox_.push(std::move(m)); }

void Coordinator::reply(const ReplyTo& to, int status, std::string body) {
  if (!to.valid() || !listener_) return;
  Response r;
  r.status = status;
  r.body = std::move(body);
  listener_->reply(to, std::move(r));
}

void Coordinator::reply_error(const ReplyTo& to, int status, std::string_view message) {
  reply(to, status, error_json(message));
}

bool Coordinator::load(std::string& err) {
  started_ms_ = wall_ms();
  CatalogData data;
  const std::string path = (fs::path(cfg_.data_dir) / "catalog.json").string();
  if (!load_catalog(path, data, err)) return false;
  for (auto& s : data.streams) create_stream(std::move(s));
  publish_routing();
  for (auto& q : data.queries) {
    Response error;
    const std::string name = q.name;
    if (!start_query(q, true, error)) {
      log::error("no se pudo restaurar la consulta '", name, "': ", error.body);
      broken_.push_back(std::move(q));
    }
  }
  if (!data.streams.empty() || !data.queries.empty())
    log::info("catálogo cargado: ", streams_.size(), " stream(s), ", queries_.size(), " consulta(s)");
  return true;
}

void Coordinator::start() {
  if (started_ms_ == 0) started_ms_ = wall_ms();
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void Coordinator::stop() {
  if (!thread_.joinable()) return;
  running_ = false;
  inbox_.wake();
  thread_.join();
}

void Coordinator::run() {
  log::set_thread_name("coordinador");
  std::vector<msg::CoordMsg> batch;
  last_tick_ = wall_ms();
  while (running_) {
    inbox_.wait_pop_all(batch, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
    for (auto& m : batch) handle(m);
    batch.clear();
    const int64_t now = wall_ms();
    if (now - last_tick_ >= kMetricsEveryMs) tick(now);
  }
}

void Coordinator::handle(msg::CoordMsg& m) {
  std::visit([this](auto& x) { on(x); }, m);
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

Coordinator::StreamState* Coordinator::create_stream(StreamConfig cfg) {
  StreamState s;
  s.cfg = std::move(cfg);
  if (s.cfg.created_ms == 0) s.cfg.created_ms = wall_ms();
  const uint64_t base = hash_str(s.cfg.name);
  for (uint32_t p = 0; p < s.cfg.partitions; ++p) {
    const uint32_t shard = static_cast<uint32_t>((base + p) % shards_.size());
    s.shard_of.push_back(shard);
    s.counters.push_back(std::make_shared<PartitionCounters>());
    shards_[shard]->post(msg::OpenPartition{s.cfg, p, s.counters.back()});
  }
  s.next_partition = std::make_shared<std::atomic<uint32_t>>(0);
  auto [it, inserted] = streams_.emplace(s.cfg.name, std::move(s));
  return &it->second;
}

void Coordinator::forward_ingest(StreamState& s, msg::Ingest&& ingest) {
  ingest.partition = s.next_partition->fetch_add(1, std::memory_order_relaxed) % s.cfg.partitions;
  shards_[s.shard_of[ingest.partition]]->post(std::move(ingest));
}

void Coordinator::on(msg::CreateStream& m) {
  auto it = streams_.find(m.cfg.name);
  if (it != streams_.end()) {
    if (m.then_ingest) {
      forward_ingest(it->second, std::move(*m.then_ingest));
      return;
    }
    if (m.if_missing) {
      reply(m.reply, 200, stream_to_json(it->second.cfg));
      return;
    }
    reply_error(m.reply, 409, cat("el stream '", m.cfg.name, "' ya existe"));
    return;
  }
  if (!valid_name(m.cfg.name)) {
    reply_error(m.reply, 400, "nombre de stream inválido: usa letras, números, '_', '-' o '.' (máx. 64)");
    return;
  }
  if (m.cfg.partitions < 1 || m.cfg.partitions > 64) {
    reply_error(m.reply, 400, "partitions debe estar entre 1 y 64");
    return;
  }
  StreamState* s = create_stream(m.cfg);
  log::info("stream '", s->cfg.name, "' creado con ", s->cfg.partitions, " partición(es)");
  publish_routing();
  persist();
  publish_catalog();
  if (m.then_ingest) {
    forward_ingest(*s, std::move(*m.then_ingest));
    return;
  }
  reply(m.reply, 201, stream_to_json(s->cfg));
}

void Coordinator::on(msg::DeleteStream& m) {
  auto it = streams_.find(m.name);
  if (it == streams_.end()) {
    reply_error(m.reply, 404, cat("no existe el stream '", m.name, "'"));
    return;
  }
  if (!it->second.queries.empty() && !m.cascade) {
    reply_error(m.reply, 409,
                cat("el stream tiene ", it->second.queries.size(),
                    " consulta(s); bórralas antes o usa ?cascade=true"));
    return;
  }
  const std::vector<std::string> qs = it->second.queries;
  for (const auto& q : qs) stop_query(q);
  for (uint32_t p = 0; p < it->second.cfg.partitions; ++p)
    shards_[it->second.shard_of[p]]->post(msg::ClosePartition{m.name, p, true});
  streams_.erase(it);
  log::info("stream '", m.name, "' borrado");
  publish_routing();
  persist();
  publish_catalog();
  reply(m.reply, 200, cat("{\"deleted\":\"", m.name, "\",\"queries_deleted\":", qs.size(), "}"));
}

// ---------------------------------------------------------------------------
// Consultas
// ---------------------------------------------------------------------------

bool Coordinator::start_query(QueryDef def, bool recovering, Response& error) {
  auto plan = std::make_shared<zql::QueryPlan>();
  zql::Diagnostic diag;
  if (!zql::compile_query(def.zql, cfg_.compile, *plan, diag)) {
    error.status = 400;
    json::Writer(error.body)
        .begin_object()
        .kv("error", diag.message)
        .kv_int("line", diag.pos.line)
        .kv_int("col", diag.pos.col)
        .kv_int("offset", static_cast<int64_t>(diag.pos.offset))
        .end_object();
    return false;
  }
  plan->name = def.name;
  auto sit = streams_.find(plan->stream);
  if (sit == streams_.end()) {
    if (!cfg_.auto_create_streams) {
      error.status = 404;
      error.body = error_json(cat("no existe el stream '", plan->stream, "'"));
      return false;
    }
    StreamConfig sc;
    sc.name = plan->stream;
    sc.partitions = cfg_.default_partitions;
    if (!valid_name(sc.name)) {
      error.status = 400;
      error.body = error_json(cat("nombre de stream inválido: '", plan->stream, "'"));
      return false;
    }
    create_stream(sc);
    publish_routing();
    sit = streams_.find(plan->stream);
  }
  const int64_t now = wall_ms();
  if (def.id == 0) def.id = next_qid_++;
  if (def.created_ms == 0) def.created_ms = now;
  bool backfill = true;
  int64_t start_ts = def.start_ms;
  const uint32_t partitions = sit->second.cfg.partitions;
  const bool resume_by_offset = recovering && counts_from_now(def.from) && def.start_offsets.size() == partitions &&
                                std::all_of(def.start_offsets.begin(), def.start_offsets.end(),
                                            [](int64_t o) { return o >= 0; });
  if (resume_by_offset) {
    // Lo mismo que contaba en vivo: todo lo que llegó desde su creación.
    start_ts = std::numeric_limits<int64_t>::min();
  } else if (recovering) {
    start_ts = std::max(def.start_ms, now - cfg_.recovery_horizon_ms);
  } else if (!resolve_start(def.from, now, def.start_ms, backfill)) {
    error.status = 400;
    error.body = error_json("'from' debe ser now, beginning, una duración (1h) o una fecha RFC 3339");
    return false;
  } else {
    // En vivo no se filtra por tiempo: cuenta todo lo que llegue desde ahora
    // (la marca de agua ya decide qué es tardío). Con backfill, desde start_ms.
    start_ts = backfill ? def.start_ms : std::numeric_limits<int64_t>::min();
  }

  auto q = std::make_unique<QueryState>();
  q->def = def;
  q->plan = plan;
  q->layout = std::make_shared<AggLayout>(plan);
  q->counters = std::make_shared<QueryCounters>();
  FinalizerOptions fo;
  fo.max_groups = cfg_.max_groups;
  fo.max_rows = cfg_.max_rows;
  fo.history = cfg_.history_windows;
  q->fin = std::make_unique<QueryFinalizer>(def.name, q->layout, sit->second.cfg.partitions, fo);
  StreamState& s = sit->second;
  for (uint32_t p = 0; p < s.cfg.partitions; ++p) {
    msg::AddQuery add;
    add.stream = s.cfg.name;
    add.partition = p;
    add.qid = def.id;
    add.layout = q->layout;
    add.counters = q->counters;
    add.start_ts = start_ts;
    add.backfill = backfill;
    if (resume_by_offset) {
      add.start_offset = def.start_offsets[p];
      add.horizon_ts = now - cfg_.recovery_horizon_ms;
    }
    shards_[s.shard_of[p]]->post(std::move(add));
  }
  s.queries.push_back(def.name);
  by_id_[def.id] = q.get();
  queries_[def.name] = std::move(q);
  return true;
}

void Coordinator::stop_query(const std::string& name) {
  auto it = queries_.find(name);
  if (it == queries_.end()) return;
  QueryState& q = *it->second;
  auto sit = streams_.find(q.plan->stream);
  if (sit != streams_.end()) {
    auto& qs = sit->second.queries;
    qs.erase(std::remove(qs.begin(), qs.end(), name), qs.end());
    for (uint32_t p = 0; p < sit->second.cfg.partitions; ++p)
      shards_[sit->second.shard_of[p]]->post(msg::RemoveQuery{q.def.id});
  }
  alerts_.forget_query(name);
  by_id_.erase(q.def.id);
  queries_.erase(it);
}

void Coordinator::on(msg::CreateQuery& m) {
  if (!valid_name(m.name)) {
    reply_error(m.reply, 400, "nombre de consulta inválido: usa letras, números, '_', '-' o '.' (máx. 64)");
    return;
  }
  if (queries_.count(m.name)) {
    reply_error(m.reply, 409, cat("ya existe una consulta llamada '", m.name, "'"));
    return;
  }
  QueryDef def;
  def.name = m.name;
  def.zql = m.zql;
  def.from = m.from.empty() ? "now" : m.from;
  Response error;
  if (!start_query(std::move(def), false, error)) {
    reply(m.reply, error.status, std::move(error.body));
    return;
  }
  const QueryState& q = *queries_[m.name];
  log::info("consulta '", m.name, "' creada sobre '", q.plan->stream, "'");
  publish_routing();
  persist();
  publish_catalog();
  reply(m.reply, 201, query_json(q, true));
}

void Coordinator::on(msg::DeleteQuery& m) {
  if (!queries_.count(m.name)) {
    reply_error(m.reply, 404, cat("no existe la consulta '", m.name, "'"));
    return;
  }
  stop_query(m.name);
  log::info("consulta '", m.name, "' borrada");
  publish_routing();
  persist();
  publish_catalog();
  reply(m.reply, 200, cat("{\"deleted\":\"", m.name, "\"}"));
}

void Coordinator::on(msg::GetQuery& m) {
  auto it = queries_.find(m.name);
  if (it == queries_.end()) {
    reply_error(m.reply, 404, cat("no existe la consulta '", m.name, "'"));
    return;
  }
  reply(m.reply, 200, query_json(*it->second, true));
}

void Coordinator::on(msg::ListQueries& m) {
  std::string out;
  json::Writer w(out);
  w.begin_array();
  for (const auto& [name, q] : queries_) w.raw(query_json(*q, false));
  w.end_array();
  reply(m.reply, 200, std::move(out));
}

void Coordinator::on(msg::GetResults& m) {
  auto it = queries_.find(m.name);
  if (it == queries_.end()) {
    reply_error(m.reply, 404, cat("no existe la consulta '", m.name, "'"));
    return;
  }
  std::string out;
  json::Writer w(out);
  w.begin_object().key("results").begin_array();
  for (const auto& r : it->second->fin->history(m.last)) w.raw(*r);
  w.end_array().key("partial");
  if (auto p = it->second->fin->latest_partial())
    w.raw(*p);
  else
    w.null();
  w.end_object();
  reply(m.reply, 200, std::move(out));
}

void Coordinator::on(msg::ListAlerts& m) { reply(m.reply, 200, alerts_.to_json()); }

void Coordinator::on(msg::MetricsHistory& m) {
  std::string out;
  json::Writer w(out);
  w.begin_array();
  for (const auto& s : metrics_) w.raw(*s);
  w.end_array();
  reply(m.reply, 200, std::move(out));
}

std::string Coordinator::query_json(const QueryState& q, bool detailed) const {
  const zql::QueryPlan& p = *q.plan;
  const QueryCounters& c = *q.counters;
  std::string out;
  json::Writer w(out);
  w.begin_object()
      .kv("name", q.def.name)
      .kv("stream", p.stream)
      .kv("zql", q.def.zql)
      .kv("from", q.def.from)
      .kv_int("start_ms", q.def.start_ms)
      .kv_int("created_ms", q.def.created_ms);
  w.key("columns").begin_array();
  for (const auto& col : p.columns) w.str(col.name);
  w.end_array();
  w.key("keys").begin_array();
  for (const auto& k : p.key_names) w.str(k);
  w.end_array();
  w.key("window")
      .begin_object()
      .kv("type", p.window_type == zql::WindowType::Tumbling ? "tumbling" : "hopping")
      .kv_int("size_ms", p.window_ms)
      .kv_int("advance_ms", p.advance_ms)
      .kv_int("lateness_ms", p.lateness_ms)
      .end_object();
  w.kv_int("emit_every_ms", p.emit_every_ms);
  w.key("alert");
  if (p.has_alert)
    w.begin_object().kv("condition", p.alert_text).kv_int("for", p.alert_for).end_object();
  else
    w.null();
  uint32_t partitions = 1;
  if (auto s = streams_.find(p.stream); s != streams_.end()) partitions = s->second.cfg.partitions;
  w.key("stats")
      .begin_object()
      .kv_int("events", static_cast<int64_t>(c.events.load()))
      .kv_int("matched", static_cast<int64_t>(c.matched.load()))
      .kv_int("late", static_cast<int64_t>(c.late.load()))
      .kv_int("dropped", static_cast<int64_t>(c.dropped.load()))
      .kv_int("lag", c.lag.load())
      .kv_int("partitions", partitions)
      .kv_int("partitions_live", c.partitions_live.load())
      .kv_int("windows", static_cast<int64_t>(c.windows.load()))
      .kv_int("partials", static_cast<int64_t>(c.partials.load()))
      .kv_int("last_window_end_ms", c.last_window_end.load())
      .kv_int("alerts_firing", static_cast<int64_t>(q.fin->alerts_firing()))
      .end_object();
  if (detailed) {
    w.kv("explain", p.explain(false));
    w.key("latest");
    if (auto l = q.fin->latest())
      w.raw(*l);
    else
      w.null();
    w.key("latest_partial");
    if (auto l = q.fin->latest_partial())
      w.raw(*l);
    else
      w.null();
  }
  w.end_object();
  return out;
}

// ---------------------------------------------------------------------------
// Paneles y resultados
// ---------------------------------------------------------------------------

void Coordinator::dispatch(QueryState& q, FinalizerOutput& out) {
  for (auto& win : out.windows) {
    if (win.partial) {
      q.counters->partials.fetch_add(1, std::memory_order_relaxed);
    } else {
      q.counters->windows.fetch_add(1, std::memory_order_relaxed);
      q.counters->last_window_end = win.end;
      ++windows_since_tick_;
    }
    if (listener_)
      listener_->publish(Publication{win.partial ? Topic::QueryPartial : Topic::QueryResult, q.def.name,
                                     win.partial ? "partial" : "result", win.json});
  }
  for (auto& a : out.alerts) {
    alerts_.apply(a);
    // Las transiciones que se recalculan al re-procesar el log tras un
    // reinicio actualizan el estado, pero no se notifican como nuevas.
    if (a.at_ms >= started_ms_ - 30000 && listener_) {
      log::info("alerta ", a.firing ? "DISPARADA" : "resuelta", ": ", a.id);
      listener_->publish(Publication{Topic::Alert, "", "alert", a.json});
    }
  }
}

void Coordinator::on(msg::PaneClosed& m) {
  auto it = by_id_.find(m.qid);
  if (it == by_id_.end()) return;
  FinalizerOutput out;
  it->second->fin->on_pane(m.partition, std::move(m.pane), out);
  dispatch(*it->second, out);
}

void Coordinator::on(msg::Watermark& m) {
  auto it = by_id_.find(m.qid);
  if (it == by_id_.end()) return;
  FinalizerOutput out;
  it->second->fin->on_watermark(m.partition, m.wm, out);
  dispatch(*it->second, out);
}

void Coordinator::on(msg::QueryStarted& m) {
  auto it = by_id_.find(m.qid);
  if (it == by_id_.end()) return;
  QueryDef& def = it->second->def;
  const auto sit = streams_.find(it->second->plan->stream);
  if (sit == streams_.end() || m.partition >= sit->second.cfg.partitions) return;
  def.start_offsets.resize(sit->second.cfg.partitions, -1);
  def.start_offsets[m.partition] = m.offset;
  // Se guarda cuando se conocen todas las particiones.
  if (std::all_of(def.start_offsets.begin(), def.start_offsets.end(), [](int64_t o) { return o >= 0; })) persist();
}

void Coordinator::on(msg::EarlyPanes& m) {
  auto it = by_id_.find(m.qid);
  if (it == by_id_.end()) return;
  FinalizerOutput out;
  it->second->fin->on_early(m.partition, std::move(m.panes), wall_ms(), out);
  dispatch(*it->second, out);
}

// ---------------------------------------------------------------------------
// Catálogo, enrutado y métricas
// ---------------------------------------------------------------------------

void Coordinator::publish_routing() {
  auto r = std::make_shared<Routing>();
  for (const auto& [name, s] : streams_) {
    auto route = std::make_shared<StreamRoute>();
    route->cfg = s.cfg;
    route->shard_of = s.shard_of;
    route->counters = s.counters;
    route->next_partition = s.next_partition;
    route->queries = s.queries;
    r->streams.emplace(name, std::move(route));
  }
  routing_->set(std::move(r));
}

void Coordinator::publish_catalog() {
  if (!listener_) return;
  listener_->publish(Publication{Topic::Catalog, "", "catalog",
                                 std::make_shared<const std::string>(cat("{\"t\":", wall_ms(), "}"))});
}

void Coordinator::persist() {
  CatalogData data;
  for (const auto& [name, s] : streams_) data.streams.push_back(s.cfg);
  for (const auto& [name, q] : queries_) data.queries.push_back(q->def);
  for (const auto& b : broken_) data.queries.push_back(b);
  std::error_code ec;
  fs::create_directories(cfg_.data_dir, ec);
  std::string err;
  if (!save_catalog((fs::path(cfg_.data_dir) / "catalog.json").string(), data, err))
    log::error("no se pudo guardar el catálogo: ", err);
}

void Coordinator::tick(int64_t now) {
  const double dt = static_cast<double>(now - last_tick_) / 1000.0;
  last_tick_ = now;
  uint64_t events = 0, bytes = 0, late = 0;
  int64_t stored = 0;
  std::string per_stream;
  json::Writer ps(per_stream);
  ps.begin_object();
  for (const auto& [name, s] : streams_) {
    uint64_t se = 0;
    for (const auto& c : s.counters) {
      se += c->events.load(std::memory_order_relaxed);
      bytes += c->bytes.load(std::memory_order_relaxed);
      stored += std::max<int64_t>(0, c->end_offset.load(std::memory_order_relaxed) - c->start_offset.load(std::memory_order_relaxed));
    }
    events += se;
    ps.key(name).integer(static_cast<int64_t>(se));
  }
  ps.end_object();
  for (const auto& [name, q] : queries_) late += q->counters->late.load(std::memory_order_relaxed);
  const double eps = dt > 0 ? static_cast<double>(events - std::min(events, last_events_)) / dt : 0;
  const double bps = dt > 0 ? static_cast<double>(bytes - std::min(bytes, last_bytes_)) / dt : 0;
  const double lps = dt > 0 ? static_cast<double>(late - std::min(late, last_late_)) / dt : 0;
  const double wps = dt > 0 ? static_cast<double>(windows_since_tick_) / dt : 0;
  last_events_ = events;
  last_bytes_ = bytes;
  last_late_ = late;
  windows_since_tick_ = 0;

  std::string out;
  json::Writer w(out);
  w.begin_object()
      .kv_int("t", now)
      .kv_num("events_per_sec", std::round(eps * 10) / 10)
      .kv_num("bytes_per_sec", std::round(bps))
      .kv_num("late_per_sec", std::round(lps * 10) / 10)
      .kv_num("windows_per_sec", std::round(wps * 10) / 10)
      .kv_int("events_total", static_cast<int64_t>(events))
      .kv_int("events_stored", stored)
      .kv_int("streams", static_cast<int64_t>(streams_.size()))
      .kv_int("queries", static_cast<int64_t>(queries_.size()))
      .kv_int("alerts_active", static_cast<int64_t>(alerts_.active()))
      .key("stream_events")
      .raw(per_stream)
      .end_object();
  auto sample = std::make_shared<const std::string>(std::move(out));
  metrics_.push_back(sample);
  while (metrics_.size() > kMetricsHistory) metrics_.pop_front();
  if (listener_) listener_->publish(Publication{Topic::Metrics, "", "metrics", sample});
}

}  // namespace zr::engine
