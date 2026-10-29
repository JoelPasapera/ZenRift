// Cola de mensajes de varios productores y un consumidor.
//
// El consumidor no saca elementos de uno en uno: intercambia el vector interno
// por uno vacío y procesa el lote completo. Con mensajes de grano grueso (un
// lote de eventos, un panel de ventana) el coste del mutex es despreciable y a
// cambio la cola es trivialmente correcta y admite espera con plazo.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace zr {

template <class T>
class MpscQueue {
 public:
  using Clock = std::chrono::steady_clock;

  // Encola. Devuelve true si la cola estaba vacía (para despertar a un
  // consumidor que duerme en epoll/poll en vez de en la variable de condición).
  bool push(T item) {
    bool was_empty;
    bool notify;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      was_empty = items_.empty();
      items_.push_back(std::move(item));
      notify = waiting_;
    }
    if (notify) cv_.notify_one();
    return was_empty;
  }

  // Mueve todo lo pendiente a `out` (que se vacía antes). Si no hay nada,
  // espera hasta `deadline` o hasta wake(). Devuelve true si obtuvo elementos.
  bool wait_pop_all(std::vector<T>& out, Clock::time_point deadline) {
    out.clear();
    std::unique_lock<std::mutex> lock(mutex_);
    if (items_.empty() && !woken_) {
      waiting_ = true;
      cv_.wait_until(lock, deadline, [&] { return !items_.empty() || woken_; });
      waiting_ = false;
    }
    woken_ = false;
    if (items_.empty()) return false;
    out.swap(items_);
    return true;
  }

  bool try_pop_all(std::vector<T>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) return false;
    out.swap(items_);
    return true;
  }

  // Interrumpe una espera en curso (o la siguiente) sin encolar nada.
  void wake() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      woken_ = true;
    }
    cv_.notify_one();
  }

  size_t approx_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<T> items_;
  bool waiting_ = false;
  bool woken_ = false;
};

}  // namespace zr
