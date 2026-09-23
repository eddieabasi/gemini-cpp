#pragma once

#include "gemini/error.hpp"

#include <chrono>

namespace gemini {

/// Exponential backoff with jitter. Server-provided hints (RetryInfo / Retry-After) win
/// over the computed delay but are still capped by `max_backoff`.
struct RetryPolicy {
  int max_attempts = 4;  // total attempts including the first one; 1 disables retries
  std::chrono::milliseconds initial_backoff{500};
  std::chrono::milliseconds max_backoff{30'000};
  double multiplier = 2.0;
  double jitter = 0.2;  // each delay is scaled by a random factor in [1 - jitter, 1 + jitter]

  [[nodiscard]] static RetryPolicy none() { return RetryPolicy{.max_attempts = 1}; }

  /// Delay before retry number `retry_index` (0 = first retry) after failure `error`.
  [[nodiscard]] std::chrono::milliseconds delay_for(int retry_index, const Error& error) const;
};

}  // namespace gemini
