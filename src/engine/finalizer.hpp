// Finalización de una consulta (lado coordinador).
//
// Recibe de cada partición los paneles cerrados y sus marcas de agua. Un panel
// se da por terminado cuando TODAS las particiones han superado su final
// (marca global = mínimo de las marcas): entonces se fusionan los parciales de
// todas las particiones, se compone la ventana (1 panel en tumbling, N en
// hopping), se evalúan SELECT / HAVING / ORDER BY / LIMIT y ALERT, y se
// produce el resultado en JSON.
//
// Con EMIT EVERY también compone resultados parciales de la ventana en curso
// a partir de las instantáneas de paneles abiertos que mandan las particiones.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/aggregate.hpp"
#include "engine/alerts.hpp"
#include "engine/pane.hpp"

namespace zr::engine {

struct FinalizerOptions {
  size_t max_groups = 100000;
  size_t max_rows = 1000;
  size_t history = 360;
  uint32_t max_empty_windows = 60;  // ventanas vacías seguidas que se emiten en consultas globales
};

struct WindowResult {
  std::shared_ptr<const std::string> json;
  bool partial = false;
  int64_t start = 0;
  int64_t end = 0;
  size_t rows = 0;
};

struct FinalizerOutput {
  std::vector<WindowResult> windows;
  std::vector<AlertTransition> alerts;
};

class QueryFinalizer {
 public:
  QueryFinalizer(std::string name, std::shared_ptr<const AggLayout> layout, uint32_t partitions,
                 const FinalizerOptions& opts);

  void on_pane(uint32_t partition, std::unique_ptr<Pane> pane, FinalizerOutput& out);
  void on_watermark(uint32_t partition, int64_t wm, FinalizerOutput& out);
  void on_early(uint32_t partition, std::vector<std::unique_ptr<Pane>> panes, int64_t now_wall,
                FinalizerOutput& out);

  // Últimos resultados finales (más antiguo primero).
  std::vector<std::shared_ptr<const std::string>> history(size_t last) const;
  std::shared_ptr<const std::string> latest() const;
  std::shared_ptr<const std::string> latest_partial() const { return latest_partial_; }
  size_t alerts_firing() const noexcept { return alerts_ ? alerts_->firing() : 0; }
  int64_t global_watermark() const noexcept;
  const std::string& name() const noexcept { return name_; }
  const zql::QueryPlan& plan() const noexcept { return layout_->plan(); }

 private:
  void finalize(FinalizerOutput& out);
  void emit_window(int64_t end, FinalizerOutput& out);
  WindowResult render(const Pane* merged, int64_t start, int64_t end, bool partial, AlertTracker* alerts,
                      std::vector<AlertTransition>* transitions);

  std::string name_;
  std::shared_ptr<const AggLayout> layout_;
  uint32_t partitions_;
  FinalizerOptions opts_;
  int64_t pane_ms_;
  int64_t window_ms_;

  std::vector<int64_t> part_wm_;
  std::vector<bool> part_has_wm_;
  std::map<int64_t, std::unique_ptr<Pane>> pending_;           // inicio -> parciales fusionados
  std::deque<std::pair<int64_t, std::unique_ptr<Pane>>> recent_;  // paneles finalizados dentro de la ventana
  std::vector<std::vector<std::unique_ptr<Pane>>> early_;      // por partición
  bool finalized_set_ = false;
  int64_t finalized_upto_ = 0;
  uint32_t empty_streak_ = 0;
  int64_t last_partial_wall_ = 0;

  std::unique_ptr<AlertTracker> alerts_;
  std::deque<std::shared_ptr<const std::string>> history_;
  std::shared_ptr<const std::string> latest_partial_;
};

}  // namespace zr::engine
