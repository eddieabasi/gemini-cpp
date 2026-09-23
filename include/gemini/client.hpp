#pragma once

#include "gemini/error.hpp"
#include "gemini/retry.hpp"
#include "gemini/transport.hpp"
#include "gemini/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gemini {

inline constexpr std::string_view kDefaultModel = "gemini-2.5-flash";
inline constexpr std::string_view kDefaultBaseUrl = "https://generativelanguage.googleapis.com/v1beta";

struct ClientOptions {
  std::string api_key;
  std::string model{kDefaultModel};
  std::string base_url{kDefaultBaseUrl};
  std::chrono::milliseconds timeout{120'000};
  RetryPolicy retry;
  /// Used between retries; injectable so tests do not actually sleep.
  std::function<void(std::chrono::milliseconds)> sleeper;

  /// Reads GEMINI_API_KEY (or GOOGLE_API_KEY), GEMINI_MODEL and GEMINI_BASE_URL.
  [[nodiscard]] static Result<ClientOptions> from_env();
};

struct MetricsSnapshot {
  std::uint64_t requests = 0;   // logical calls (generate/stream/count/list)
  std::uint64_t attempts = 0;   // HTTP attempts including retries
  std::uint64_t retries = 0;
  std::uint64_t failures = 0;   // logical calls that ultimately failed
  std::uint64_t prompt_tokens = 0;
  std::uint64_t output_tokens = 0;
  std::uint64_t total_latency_ms = 0;
};

/// Thread-safe Gemini API client.
class Client {
 public:
  /// Called for each streamed chunk. Return false to cancel the stream.
  using StreamCallback = std::function<bool(const GenerateResponse& chunk)>;

  explicit Client(ClientOptions options);
  Client(ClientOptions options, std::unique_ptr<Transport> transport);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  [[nodiscard]] Result<GenerateResponse> generate(const GenerateRequest& request);
  [[nodiscard]] Result<GenerateResponse> generate(std::string_view prompt);

  /// Streams the response via server-sent events. Returns the aggregated response.
  /// Retries only happen if the failure occurs before the first chunk was delivered, so
  /// callers never see duplicated output. Cancelling returns the partial aggregate.
  [[nodiscard]] Result<GenerateResponse> stream(const GenerateRequest& request,
                                                const StreamCallback& on_chunk);

  [[nodiscard]] Result<std::int64_t> count_tokens(const GenerateRequest& request);
  [[nodiscard]] Result<std::vector<ModelInfo>> list_models();

  [[nodiscard]] const std::string& model() const noexcept { return options_.model; }
  [[nodiscard]] MetricsSnapshot metrics() const noexcept;

 private:
  struct Metrics {
    std::atomic<std::uint64_t> requests{0}, attempts{0}, retries{0}, failures{0};
    std::atomic<std::uint64_t> prompt_tokens{0}, output_tokens{0}, total_latency_ms{0};
  };

  template <typename T, typename Attempt, typename CanRetry>
  Result<T> with_retry(std::string_view op, Attempt&& attempt, CanRetry&& can_retry);

  [[nodiscard]] std::string endpoint(std::string_view method, bool sse = false) const;
  [[nodiscard]] HttpRequest make_request(std::string url, std::string body) const;
  [[nodiscard]] Status validate(const GenerateRequest& request) const;
  void record(std::string_view op, std::chrono::steady_clock::time_point start,
              const Result<GenerateResponse>& result);

  ClientOptions options_;
  std::unique_ptr<Transport> transport_;
  Metrics metrics_;
};

}  // namespace gemini
