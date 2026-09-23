#include "gemini/retry.hpp"

#include <gtest/gtest.h>

using namespace gemini;
using std::chrono::milliseconds;

TEST(Retry, ExponentialWithoutJitter) {
  RetryPolicy p{.max_attempts = 5, .initial_backoff = milliseconds(100), .max_backoff = milliseconds(1000),
                .multiplier = 2.0, .jitter = 0.0};
  Error e{ErrorCode::kServer, "x", 503, {}, std::nullopt};
  EXPECT_EQ(p.delay_for(0, e), milliseconds(100));
  EXPECT_EQ(p.delay_for(1, e), milliseconds(200));
  EXPECT_EQ(p.delay_for(2, e), milliseconds(400));
  EXPECT_EQ(p.delay_for(4, e), milliseconds(1000));  // capped
}

TEST(Retry, JitterStaysInBounds) {
  RetryPolicy p{.initial_backoff = milliseconds(1000), .jitter = 0.2};
  Error e{ErrorCode::kTransport, "x", 0, {}, std::nullopt};
  for (int i = 0; i < 1000; ++i) {
    const auto d = p.delay_for(0, e);
    EXPECT_GE(d, milliseconds(800));
    EXPECT_LE(d, milliseconds(1200));
  }
}

TEST(Retry, ServerHintWinsButIsCapped) {
  RetryPolicy p{.max_backoff = milliseconds(5000)};
  Error e{ErrorCode::kRateLimited, "x", 429, {}, milliseconds(1500)};
  EXPECT_EQ(p.delay_for(3, e), milliseconds(1500));
  e.retry_after = milliseconds(60000);
  EXPECT_EQ(p.delay_for(0, e), milliseconds(5000));
}

TEST(Retry, RetryableClassification) {
  for (auto code : {ErrorCode::kTransport, ErrorCode::kTimeout, ErrorCode::kRateLimited, ErrorCode::kServer}) {
    EXPECT_TRUE((Error{code, "", 0, {}, std::nullopt}.retryable()));
  }
  for (auto code : {ErrorCode::kInvalidArgument, ErrorCode::kUnauthenticated, ErrorCode::kNotFound,
                    ErrorCode::kParse, ErrorCode::kBlocked, ErrorCode::kCancelled, ErrorCode::kConfig}) {
    EXPECT_FALSE((Error{code, "", 0, {}, std::nullopt}.retryable()));
  }
}
