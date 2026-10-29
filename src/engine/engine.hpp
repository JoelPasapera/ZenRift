// Fachada del motor: arranca el coordinador y los shards, y ofrece a la capa
// de red (o a cualquier productor en proceso) operaciones asíncronas cuya
// respuesta llega por EngineListener::reply.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/coordinator.hpp"
#include "engine/routing.hpp"
#include "engine/shard.hpp"
#include "engine/types.hpp"

namespace zr::engine {

class Engine {
 public:
  Engine(EngineConfig cfg, EngineListener* listener);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  bool start(std::string& err);
  void stop();

  // --- Operaciones (seguras entre hilos salvo los tails) ---
  void ingest(std::string_view stream, std::string body, ReplyTo reply);
  void create_stream(StreamConfig cfg, ReplyTo reply);
  void delete_stream(std::string name, bool cascade, ReplyTo reply);
  void create_query(std::string name, std::string zql, std::string from, ReplyTo reply);
  void delete_query(std::string name, ReplyTo reply);
  void get_query(std::string name, ReplyTo reply);
  void list_queries(ReplyTo reply);
  void get_results(std::string name, uint32_t last, ReplyTo reply);
  void list_alerts(ReplyTo reply);
  void metrics_history(ReplyTo reply);
  // false si el stream o la partición no existen.
  bool read_events(const std::string& stream, uint32_t partition, int64_t from, uint32_t limit, ReplyTo reply);

  // Tails en vivo (solo desde el hilo de red).
  bool add_tail(uint64_t tail_id, const std::string& stream, std::shared_ptr<const zql::FilterPlan> filter);
  void remove_tail(uint64_t tail_id);

  // Lecturas sin ida y vuelta (instantánea de enrutado + contadores atómicos).
  std::string streams_json() const;
  std::string stream_json(const std::string& name, bool& found) const;
  std::string stats_json() const;

  const EngineConfig& config() const noexcept { return cfg_; }
  uint32_t shard_count() const noexcept { return static_cast<uint32_t>(shards_.size()); }

 private:
  EngineConfig cfg_;
  EngineListener* listener_;
  RoutingTable routing_;
  std::unique_ptr<Coordinator> coord_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> tail_shards_;  // tail -> shards donde está
  int64_t started_ms_ = 0;
  bool running_ = false;
};

}  // namespace zr::engine
