#include "gemini/retry.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace gemini {

std::chrono::milliseconds RetryPolicy::delay_for(int retry_index, const Error& error) const {
  using std::chrono::milliseconds;
  if (error.retry_after) return std::min(*error.retry_after, max_backoff);

  const double base = static_cast<double>(initial_backoff.count()) *
                      std::pow(multiplier, static_cast<double>(std::max(retry_index, 0)));
  double delay = std::min(base, static_cast<double>(max_backoff.count()));
  if (jitter > 0.0) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(1.0 - jitter, 1.0 + jitter);
    delay *= dist(rng);
  }
  return milliseconds(static_cast<milliseconds::rep>(std::max(delay, 0.0)));
}

}  // namespace gemini
