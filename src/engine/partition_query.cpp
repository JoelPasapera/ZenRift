#include "engine/partition_query.hpp"

#include <algorithm>

#include "core/hash.hpp"
#include "core/time.hpp"
#include "zql/vm.hpp"

namespace zr::engine {

PartitionQuery::PartitionQuery(uint64_t qid, uint32_t partition, std::shared_ptr<const AggLayout> layout,
                               std::shared_ptr<QueryCounters> counters, int64_t start_ts, int64_t now_wall,
                               const RuntimeOptions& opts, QuerySink* sink)
    : qid_(qid),
      partition_(partition),
      layout_(std::move(layout)),
      counters_(std::move(counters)),
      start_ts_(start_ts),
      opts_(opts),
      sink_(sink),
      pane_ms_(layout_->plan().advance_ms),
      created_wall_(now_wall),
      last_early_wall_(now_wall) {}

void PartitionQuery::bind(const FieldLayout& fields) {
  const auto& paths = plan().fields;
  field_map_.assign(paths.size(), 0);
  for (size_t i = 0; i < paths.size(); ++i) {
    const int32_t slot = fields.slot_of(paths[i]);
    // La partición siempre incluye los campos de sus consultas (ver Shard).
    field_map_[i] = slot >= 0 ? static_cast<uint32_t>(slot) : 0;
  }
}

Pane* PartitionQuery::pane_for(int64_t start) {
  if (last_pane_ && last_pane_->start() == start) return last_pane_;
  auto it = open_.find(start);
  if (it == open_.end()) it = open_.emplace(start, std::make_unique<Pane>(layout_, start, start + pane_ms_)).first;
  last_pane_ = it->second.get();
  return last_pane_;
}

void PartitionQuery::on_event(const EventView& ev, int64_t now_wall, Arena& scratch) {
  counters_->events.fetch_add(1, std::memory_order_relaxed);
  if (ev.ts < start_ts_) return;  // anterior al inicio pedido (backfill/recuperación)
  if (!has_ts_ || ev.ts > max_ts_) max_ts_ = ev.ts;
  has_ts_ = true;
  last_event_wall_ = now_wall;

  const zql::QueryPlan& p = plan();
  zql::EvalContext ctx;
  ctx.fields = ev.slots;
  ctx.field_map = field_map_.data();
  ctx.scratch = &scratch;
  if (p.has_where && !zql::eval_true(p.where, ctx)) return;
  counters_->matched.fetch_add(1, std::memory_order_relaxed);

  const int64_t start = align_down(ev.ts, pane_ms_);
  if (closed_set_ && start + pane_ms_ <= closed_upto_) {
    counters_->late.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Pane* pane = pane_for(start);

  key_buf_.clear();
  for (const auto& kp : p.keys) zql::encode_value(zql::eval(kp, ctx), key_buf_);
  char* block = pane->group(key_buf_, hash_bytes(key_buf_.data(), key_buf_.size()), opts_.max_groups);
  if (!block) {
    ++pane->dropped_groups;
    counters_->dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (const auto& e : layout_->entries()) {
    const zql::AggSpec& spec = *e.spec;
    const zql::Value v = spec.has_arg ? zql::eval(spec.arg, ctx) : zql::Value::null();
    e.ops->update(block + e.offset, v, ev.ts, spec);
  }
  ++pane->events;
}

void PartitionQuery::advance(int64_t now_wall) {
  // Mientras se pone al día desde el log, el tiempo solo avanza con los eventos.
  if (!live) {
    if (!has_ts_) return;
  } else if (!has_ts_) {
    // Sin ningún evento todavía: tras `idle_ms` el reloj de pared toma el relevo.
    if (now_wall - created_wall_ < opts_.idle_ms) return;
    max_ts_ = now_wall - opts_.idle_ms;
    last_event_wall_ = max_ts_;
    has_ts_ = true;
  }
  int64_t virtual_max = max_ts_;
  if (live && now_wall - last_event_wall_ > opts_.idle_ms) {
    // Inactiva: el tiempo de evento avanza al ritmo del reloj desde el último
    // evento (tolera productores con el reloj algo desfasado). Si lo último
    // visto es muy antiguo (datos históricos, backfill terminado), se salta
    // directamente al presente para cerrar sus ventanas sin esperar.
    virtual_max += now_wall - last_event_wall_ - opts_.idle_ms;
    if (now_wall - opts_.idle_ms - max_ts_ > opts_.skew_tolerance_ms)
      virtual_max = std::max(virtual_max, now_wall - opts_.idle_ms);
  }
  const int64_t wm = virtual_max - plan().lateness_ms;
  const int64_t boundary = align_down(wm, pane_ms_);
  if (!closed_set_ || boundary > closed_upto_) {
    while (!open_.empty() && open_.begin()->second->end() <= boundary) {
      auto node = open_.extract(open_.begin());
      if (last_pane_ == node.mapped().get()) last_pane_ = nullptr;
      sink_->pane_closed(qid_, partition_, std::move(node.mapped()));
    }
    closed_upto_ = boundary;
    closed_set_ = true;
    sink_->watermark(qid_, partition_, boundary);
  }
  const int64_t every = plan().emit_every_ms;
  if (live && every > 0 && now_wall - last_early_wall_ >= every) {
    last_early_wall_ = now_wall;
    std::vector<std::unique_ptr<Pane>> snapshot;
    snapshot.reserve(open_.size());
    for (const auto& [start, pane] : open_) snapshot.push_back(pane->clone());
    sink_->early_panes(qid_, partition_, std::move(snapshot));
  }
}

}  // namespace zr::engine
