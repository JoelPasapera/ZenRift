#include "engine/alerts.hpp"

#include "core/hash.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "json/writer.hpp"

namespace zr::engine {

void AlertTracker::begin_window() {
  for (auto& [k, st] : states_) {
    st.seen = false;
    st.cond = false;
  }
}

void AlertTracker::observe(std::string_view key, bool condition, std::string_view group_json,
                           std::string_view values_json) {
  // Solo interesa recordar grupos que cumplen la condición o ya están disparados.
  auto it = states_.find(std::string(key));
  if (it == states_.end()) {
    if (!condition) return;
    it = states_.emplace(std::string(key), State{}).first;
  }
  State& st = it->second;
  st.seen = true;
  st.cond = condition;
  // Si la fila no cumple la condición no se serializa: se conservan los
  // valores de la última vez que sí la cumplió (son los que explican la alerta).
  if (!group_json.empty()) st.group_json.assign(group_json);
  if (!values_json.empty()) st.values_json.assign(values_json);
}

std::string AlertTracker::make_json(const std::string& id, const State& st, bool firing, int64_t at) const {
  std::string out;
  json::Writer w(out);
  w.begin_object()
      .kv("id", id)
      .kv("query", query_)
      .kv("state", firing ? "firing" : "resolved")
      .kv("condition", condition_)
      .key("group")
      .raw(st.group_json.empty() ? std::string_view("{}") : std::string_view(st.group_json))
      .key("values")
      .raw(st.values_json.empty() ? std::string_view("{}") : std::string_view(st.values_json))
      .kv("at", format_rfc3339(at))
      .kv_int("at_ms", at)
      .kv("since", format_rfc3339(st.since))
      .kv_int("since_ms", st.since)
      .end_object();
  return out;
}

void AlertTracker::end_window(int64_t window_end_ms, std::vector<AlertTransition>& out) {
  for (auto it = states_.begin(); it != states_.end();) {
    State& st = it->second;
    std::string hex;
    const uint64_t h = hash_str(it->first);
    static constexpr char digits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) hex.push_back(digits[(h >> (i * 4)) & 15]);
    const std::string id = cat(query_, ":", hex);
    if (st.seen && st.cond) {
      if (st.streak == 0) st.since = window_end_ms;
      ++st.streak;
      if (!st.firing && st.streak >= for_) {
        st.firing = true;
        out.push_back({id, true, window_end_ms, std::make_shared<const std::string>(make_json(id, st, true, window_end_ms))});
      }
      ++it;
      continue;
    }
    st.streak = 0;
    if (st.firing) {
      st.firing = false;
      out.push_back({id, false, window_end_ms, std::make_shared<const std::string>(make_json(id, st, false, window_end_ms))});
    }
    it = states_.erase(it);
  }
}

size_t AlertTracker::firing() const noexcept {
  size_t n = 0;
  for (const auto& [k, st] : states_) n += st.firing;
  return n;
}

void AlertBook::apply(const AlertTransition& t) {
  if (t.firing)
    active_[t.id] = t.json;
  else
    active_.erase(t.id);
  history_.push_front(t.json);
  while (history_.size() > max_history_) history_.pop_back();
}

void AlertBook::forget_query(const std::string& query) {
  const std::string prefix = query + ":";
  for (auto it = active_.begin(); it != active_.end();) {
    if (it->first.compare(0, prefix.size(), prefix) == 0)
      it = active_.erase(it);
    else
      ++it;
  }
}

std::string AlertBook::to_json() const {
  std::string out;
  json::Writer w(out);
  w.begin_object().key("active").begin_array();
  for (const auto& [id, j] : active_) w.raw(*j);
  w.end_array().key("history").begin_array();
  for (const auto& j : history_) w.raw(*j);
  w.end_array().end_object();
  return out;
}

}  // namespace zr::engine
