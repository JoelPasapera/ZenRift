// Alertas con histéresis (ALERT IF <cond> FOR n WINDOWS).
//
// Por cada grupo se cuenta cuántas ventanas seguidas cumple la condición; al
// llegar a n se dispara ("firing") y se resuelve en la primera ventana que no
// la cumple (o en la que el grupo no aparece). Así un pico aislado no genera
// ruido y una alerta no parpadea.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zr::engine {

struct AlertTransition {
  std::string id;
  bool firing = false;
  int64_t at_ms = 0;
  std::shared_ptr<const std::string> json;
};

class AlertTracker {
 public:
  AlertTracker(std::string query, std::string condition, uint32_t for_windows)
      : query_(std::move(query)), condition_(std::move(condition)), for_(for_windows < 1 ? 1 : for_windows) {}

  void begin_window();
  // `group_json` y `values_json` son objetos JSON ya serializados.
  void observe(std::string_view key, bool condition, std::string_view group_json, std::string_view values_json);
  void end_window(int64_t window_end_ms, std::vector<AlertTransition>& out);

  size_t firing() const noexcept;

 private:
  struct State {
    uint32_t streak = 0;
    bool firing = false;
    bool seen = false;
    bool cond = false;
    int64_t since = 0;
    std::string group_json;
    std::string values_json;
  };

  std::string make_json(const std::string& id, const State& st, bool firing, int64_t at) const;

  std::string query_;
  std::string condition_;
  uint32_t for_;
  std::unordered_map<std::string, State> states_;
};

// Registro global de alertas activas e historial reciente.
class AlertBook {
 public:
  explicit AlertBook(size_t history = 200) : max_history_(history) {}
  void apply(const AlertTransition& t);
  void forget_query(const std::string& query);
  // {"active":[...],"history":[...]}
  std::string to_json() const;
  size_t active() const noexcept { return active_.size(); }

 private:
  size_t max_history_;
  std::unordered_map<std::string, std::shared_ptr<const std::string>> active_;
  std::deque<std::shared_ptr<const std::string>> history_;
};

}  // namespace zr::engine
