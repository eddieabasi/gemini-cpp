#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

class TokenBucket {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  TokenBucket(double rate_per_second, double burst,
              Clock clock = [] { return std::chrono::steady_clock::now(); })
      : rate_(rate_per_second), burst_(burst), tokens_(burst), clock_(std::move(clock)), last_(clock_()) {}

  bool try_acquire(double tokens = 1.0) {
    std::lock_guard lock(mutex_);
    refill();
    if (tokens > tokens_) return false;
    tokens_ -= tokens;
    return true;
  }

  double available() {
    std::lock_guard lock(mutex_);
    refill();
    return tokens_;
  }

  std::chrono::nanoseconds time_until_available(double tokens = 1.0) {
    std::lock_guard lock(mutex_);
    refill();
    if (tokens <= tokens_) return std::chrono::nanoseconds::zero();
    if (tokens > burst_ || rate_ <= 0.0) return std::chrono::nanoseconds::max();
    const double ns = std::ceil((tokens - tokens_) / rate_ * 1e9);
    return std::chrono::nanoseconds(static_cast<std::int64_t>(ns));
  }

 private:
  void refill() {
    const auto now = clock_();
    if (now <= last_) return;  // clock went backwards or did not move
    const double seconds = std::chrono::duration<double>(now - last_).count();
    tokens_ = std::min(burst_, tokens_ + seconds * rate_);
    last_ = now;
  }

  std::mutex mutex_;
  double rate_;
  double burst_;
  double tokens_;
  Clock clock_;
  std::chrono::steady_clock::time_point last_;
};
