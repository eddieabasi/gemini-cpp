#pragma once

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace gemini {

/// Coarse classification of everything that can go wrong talking to the API.
/// The classification drives retry decisions, so keep it stable.
enum class ErrorCode {
  kTransport,        // DNS, TLS, connection reset, ...
  kTimeout,          // request or stream stalled past its deadline
  kCancelled,        // caller aborted (e.g. Ctrl-C during streaming)
  kInvalidArgument,  // HTTP 400
  kUnauthenticated,  // HTTP 401 / 403
  kNotFound,         // HTTP 404 (usually a bad model name)
  kRateLimited,      // HTTP 429 / RESOURCE_EXHAUSTED
  kServer,           // HTTP 5xx
  kHttp,             // any other non-2xx status
  kParse,            // malformed JSON or unexpected response shape
  kBlocked,          // prompt or response blocked by safety filters
  kConfig,           // missing API key, bad option, ...
  kIo,               // local filesystem / process errors
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

struct Error {
  ErrorCode code = ErrorCode::kTransport;
  std::string message;
  int http_status = 0;
  std::string api_status;  // Google RPC status, e.g. "RESOURCE_EXHAUSTED"
  std::optional<std::chrono::milliseconds> retry_after;  // server-provided hint

  /// True for failures that are plausibly transient.
  [[nodiscard]] bool retryable() const noexcept;
  /// Human readable one-liner, e.g. "[rate_limited] Quota exceeded (HTTP 429 RESOURCE_EXHAUSTED)".
  [[nodiscard]] std::string describe() const;
};

template <typename T>
using Result = std::expected<T, Error>;

/// Result<void> helper for functions that only report success/failure.
using Status = std::expected<void, Error>;

[[nodiscard]] inline std::unexpected<Error> make_error(ErrorCode code, std::string message,
                                                       int http_status = 0) {
  return std::unexpected(Error{code, std::move(message), http_status, {}, std::nullopt});
}

}  // namespace gemini
