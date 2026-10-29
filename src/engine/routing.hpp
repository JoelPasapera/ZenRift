// Tabla de enrutado: qué shard es dueño de cada partición de cada stream.
//
// La publica el coordinador como instantánea inmutable (copy-on-write) y la
// lee el hilo de red en cada ingesta sin pedir nada a nadie.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/types.hpp"

namespace zr::engine {

struct StreamRoute {
  StreamConfig cfg;
  std::vector<uint32_t> shard_of;  // partición -> shard
  std::vector<std::shared_ptr<PartitionCounters>> counters;
  std::shared_ptr<std::atomic<uint32_t>> next_partition;  // reparto round-robin de lotes
  std::vector<std::string> queries;
};

struct Routing {
  std::map<std::string, std::shared_ptr<const StreamRoute>> streams;
};

class RoutingTable {
 public:
  RoutingTable() : current_(std::make_shared<const Routing>()) {}

  std::shared_ptr<const Routing> get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
  }

  void set(std::shared_ptr<const Routing> r) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(r);
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const Routing> current_;
};

}  // namespace zr::engine
