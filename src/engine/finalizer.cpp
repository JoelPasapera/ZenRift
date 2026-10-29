#include "engine/finalizer.hpp"

#include <algorithm>
#include <limits>

#include "core/arena.hpp"
#include "core/time.hpp"
#include "json/writer.hpp"
#include "zql/vm.hpp"

namespace zr::engine {

using zql::Value;

QueryFinalizer::QueryFinalizer(std::string name, std::shared_ptr<const AggLayout> layout, uint32_t partitions,
                               const FinalizerOptions& opts)
    : name_(std::move(name)),
      layout_(std::move(layout)),
      partitions_(partitions < 1 ? 1 : partitions),
      opts_(opts),
      pane_ms_(layout_->plan().advance_ms),
      window_ms_(layout_->plan().window_ms),
      part_wm_(partitions_, std::numeric_limits<int64_t>::min()),
      part_has_wm_(partitions_, false),
      early_(partitions_) {
  if (layout_->plan().has_alert)
    alerts_ = std::make_unique<AlertTracker>(name_, layout_->plan().alert_text, layout_->plan().alert_for);
}

int64_t QueryFinalizer::global_watermark() const noexcept {
  int64_t g = std::numeric_limits<int64_t>::max();
  for (uint32_t i = 0; i < partitions_; ++i) {
    if (!part_has_wm_[i]) return std::numeric_limits<int64_t>::min();
    g = std::min(g, part_wm_[i]);
  }
  return g;
}

void QueryFinalizer::on_pane(uint32_t partition, std::unique_ptr<Pane> pane, FinalizerOutput& out) {
  (void)out;
  if (partition >= partitions_ || !pane) return;
  // La partición cerró este panel: su instantánea parcial queda obsoleta.
  auto& early = early_[partition];
  early.erase(std::remove_if(early.begin(), early.end(),
                             [&](const std::unique_ptr<Pane>& e) { return e->start() <= pane->start(); }),
              early.end());
  if (finalized_set_ && pane->end() <= finalized_upto_) return;  // ya finalizado: no debería ocurrir
  auto it = pending_.find(pane->start());
  if (it == pending_.end())
    pending_.emplace(pane->start(), std::move(pane));
  else
    it->second->merge_from(*pane, opts_.max_groups);
}

void QueryFinalizer::on_watermark(uint32_t partition, int64_t wm, FinalizerOutput& out) {
  if (partition >= partitions_) return;
  if (!part_has_wm_[partition] || wm > part_wm_[partition]) part_wm_[partition] = wm;
  part_has_wm_[partition] = true;
  finalize(out);
}

void QueryFinalizer::finalize(FinalizerOutput& out) {
  const int64_t global = global_watermark();
  if (global == std::numeric_limits<int64_t>::min()) return;
  if (!finalized_set_) {
    finalized_upto_ = pending_.empty() ? align_down(global, pane_ms_) : std::min(global, pending_.begin()->first);
    finalized_upto_ = align_down(finalized_upto_, pane_ms_);
    finalized_set_ = true;
  }
  while (finalized_upto_ + pane_ms_ <= global) {
    const int64_t start = finalized_upto_;
    const int64_t end = start + pane_ms_;
    while (!pending_.empty() && pending_.begin()->first < start) pending_.erase(pending_.begin());
    if (!pending_.empty() && pending_.begin()->first == start) {
      recent_.emplace_back(start, std::move(pending_.begin()->second));
      pending_.erase(pending_.begin());
    }
    while (!recent_.empty() && recent_.front().first < end - window_ms_) recent_.pop_front();
    finalized_upto_ = end;
    if (!recent_.empty()) {
      empty_streak_ = 0;
      emit_window(end, out);
      continue;
    }
    if (plan().global() && empty_streak_ < opts_.max_empty_windows) {
      ++empty_streak_;
      emit_window(end, out);
      continue;
    }
    // Hueco sin datos: resolver alertas una vez y saltar hasta el próximo dato.
    if (alerts_ && alerts_->firing() > 0) {
      alerts_->begin_window();
      alerts_->end_window(end, out.alerts);
    }
    int64_t target = align_down(global, pane_ms_);
    if (!pending_.empty()) target = std::min(target, pending_.begin()->first);
    if (target > finalized_upto_) finalized_upto_ = target;
  }
}

void QueryFinalizer::emit_window(int64_t end, FinalizerOutput& out) {
  const int64_t start = end - window_ms_;
  std::unique_ptr<Pane> owned;
  const Pane* merged = nullptr;
  if (recent_.size() == 1) {
    merged = recent_.front().second.get();
  } else if (recent_.size() > 1) {
    owned = std::make_unique<Pane>(layout_, start, end);
    for (const auto& [s, p] : recent_) owned->merge_from(*p, opts_.max_groups);
    merged = owned.get();
  }
  WindowResult r = render(merged, start, end, false, alerts_.get(), &out.alerts);
  history_.push_back(r.json);
  while (history_.size() > opts_.history) history_.pop_front();
  out.windows.push_back(std::move(r));
}

void QueryFinalizer::on_early(uint32_t partition, std::vector<std::unique_ptr<Pane>> panes, int64_t now_wall,
                              FinalizerOutput& out) {
  if (partition >= partitions_) return;
  early_[partition] = std::move(panes);
  const int64_t every = plan().emit_every_ms;
  if (every <= 0 || now_wall - last_partial_wall_ < every / 2) return;
  int64_t latest = std::numeric_limits<int64_t>::min();
  for (const auto& list : early_)
    for (const auto& p : list) latest = std::max(latest, p->start());
  if (latest == std::numeric_limits<int64_t>::min()) return;
  last_partial_wall_ = now_wall;
  const int64_t end = latest + pane_ms_;
  const int64_t start = end - window_ms_;
  Pane merged(layout_, start, end);
  for (const auto& [s, p] : recent_)
    if (s >= start) merged.merge_from(*p, opts_.max_groups);
  for (const auto& [s, p] : pending_)
    if (s >= start && s < end) merged.merge_from(*p, opts_.max_groups);
  for (const auto& list : early_)
    for (const auto& p : list)
      if (p->start() >= start && p->start() < end) merged.merge_from(*p, opts_.max_groups);
  WindowResult r = render(&merged, start, end, true, nullptr, nullptr);
  latest_partial_ = r.json;
  out.windows.push_back(std::move(r));
}

std::vector<std::shared_ptr<const std::string>> QueryFinalizer::history(size_t last) const {
  const size_t n = std::min(last, history_.size());
  return {history_.end() - static_cast<std::ptrdiff_t>(n), history_.end()};
}

std::shared_ptr<const std::string> QueryFinalizer::latest() const {
  return history_.empty() ? nullptr : history_.back();
}

namespace {

struct Row {
  std::vector<Value> keys;
  std::vector<Value> cols;
  std::vector<Value> order;
};

void write_value(json::Writer& w, const Value& v, std::string& tmp) {
  tmp.clear();
  zql::write_json(tmp, v);
  w.raw(tmp);
}

}  // namespace

WindowResult QueryFinalizer::render(const Pane* merged, int64_t start, int64_t end, bool partial,
                                    AlertTracker* alerts, std::vector<AlertTransition>* transitions) {
  const zql::QueryPlan& plan = layout_->plan();
  Arena scratch(64 * 1024);
  ResultContext rctx{window_ms_, &scratch};
  zql::EvalContext ctx;
  ctx.window_start = start;
  ctx.window_end = end;
  ctx.scratch = &scratch;
  std::vector<Value> keys(plan.keys.size()), aggs(plan.aggs.size()), cols(plan.columns.size());
  std::vector<Row> rows;
  size_t groups = 0;
  std::string tmp;
  if (alerts) alerts->begin_window();

  auto process = [&](std::string_view key_bytes, const char* block) {
    ++groups;
    size_t pos = 0;
    for (auto& k : keys)
      if (!zql::decode_value(key_bytes, pos, k)) k = Value::null();
    const auto& entries = layout_->entries();
    for (size_t i = 0; i < entries.size(); ++i)
      aggs[i] = entries[i].ops->result(block + entries[i].offset, *entries[i].spec, rctx);
    ctx.keys = keys.data();
    ctx.aggs = aggs.data();
    ctx.cols = cols.data();
    for (size_t j = 0; j < cols.size(); ++j) cols[j] = zql::eval(plan.columns[j].prog, ctx);
    if (alerts) {
      const bool cond = zql::eval_true(plan.alert, ctx);
      std::string group_json, values_json;
      if (cond) {
        json::Writer g(group_json);
        g.begin_object();
        for (size_t i = 0; i < keys.size(); ++i) {
          g.key(plan.key_names[i]);
          write_value(g, keys[i], tmp);
        }
        g.end_object();
        json::Writer v(values_json);
        v.begin_object();
        for (size_t j = 0; j < cols.size(); ++j) {
          v.key(plan.columns[j].name);
          write_value(v, cols[j], tmp);
        }
        v.end_object();
      }
      alerts->observe(key_bytes, cond, group_json, values_json);
    }
    if (plan.has_having && !zql::eval_true(plan.having, ctx)) return;
    Row r;
    r.keys = keys;
    r.cols = cols;
    r.order.reserve(plan.order.size());
    for (const auto& o : plan.order) r.order.push_back(zql::eval(o.prog, ctx));
    rows.push_back(std::move(r));
  };

  if (merged) merged->for_each(process);
  if (plan.global() && groups == 0) {
    // Ventana sin eventos en una consulta global: una fila con los agregados vacíos.
    char* block = static_cast<char*>(scratch.alloc(layout_->block_size(), layout_->block_align()));
    layout_->init_block(block);
    process(std::string_view(), block);
    layout_->destroy_block(block);
  }
  if (alerts && transitions) alerts->end_window(end, *transitions);

  // Orden: ORDER BY si lo hay; si no, por clave de grupo (determinista).
  if (!plan.order.empty()) {
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
      for (size_t i = 0; i < plan.order.size(); ++i) {
        const int c = zql::total_order(a.order[i], b.order[i]);
        if (c != 0) return plan.order[i].desc ? c > 0 : c < 0;
      }
      return false;
    });
  } else if (!plan.keys.empty()) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      for (size_t i = 0; i < a.keys.size(); ++i) {
        const int c = zql::total_order(a.keys[i], b.keys[i]);
        if (c != 0) return c < 0;
      }
      return false;
    });
  }
  const size_t limit = plan.limit > 0 ? static_cast<size_t>(plan.limit) : opts_.max_rows;
  const bool truncated = rows.size() > limit;
  if (truncated) rows.resize(limit);

  std::string out;
  out.reserve(256 + rows.size() * 64);
  json::Writer w(out);
  w.begin_object()
      .kv("query", name_)
      .kv_bool("partial", partial)
      .kv("start", format_rfc3339(start))
      .kv("end", format_rfc3339(end))
      .kv_int("start_ms", start)
      .kv_int("end_ms", end)
      .kv_int("events", merged ? static_cast<int64_t>(merged->events) : 0)
      .kv_int("groups", static_cast<int64_t>(merged ? merged->groups() : 0))
      .kv_bool("truncated", truncated);
  w.key("columns").begin_array();
  for (const auto& c : plan.columns) w.str(c.name);
  w.end_array();
  w.key("rows").begin_array();
  for (const auto& r : rows) {
    w.begin_array();
    for (const auto& v : r.cols) write_value(w, v, tmp);
    w.end_array();
  }
  w.end_array().end_object();

  WindowResult res;
  res.json = std::make_shared<const std::string>(std::move(out));
  res.partial = partial;
  res.start = start;
  res.end = end;
  res.rows = rows.size();
  return res;
}

}  // namespace zr::engine
