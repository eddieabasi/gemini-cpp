#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <utility>

template <typename T, std::size_t Capacity>
class SpscQueue {
  static_assert(Capacity > 0, "capacity must be positive");

 public:
  SpscQueue() = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  ~SpscQueue() {
    while (try_pop()) {
    }
  }

  bool try_push(T value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == Capacity) return false;
    ::new (slot(tail)) T(std::move(value));
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::optional<T> try_pop() {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return std::nullopt;
    T* item = std::launder(slot(head));
    std::optional<T> out(std::move(*item));
    item->~T();
    head_.store(head + 1, std::memory_order_release);
    return out;
  }

  std::size_t size_approx() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return tail - head;  // head is read first, so tail >= head
  }

  static constexpr std::size_t capacity() { return Capacity; }

 private:
  T* slot(std::size_t counter) { return reinterpret_cast<T*>(&storage_[(counter % Capacity) * sizeof(T)]); }

  alignas(T) unsigned char storage_[Capacity * sizeof(T)];
  // Monotonic counters; unsigned wrap-around keeps the arithmetic correct.
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};
