#pragma once

#include "gemini/error.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gemini {

/// Response headers with lower-cased names.
using HttpHeaders = std::map<std::string, std::string>;

struct HttpRequest {
  std::string method = "POST";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  /// For unary requests: total deadline. For streaming requests: maximum time without
  /// receiving any bytes (a stall timeout), since healthy streams can run for minutes.
  std::chrono::milliseconds timeout{120'000};
};

struct HttpResponse {
  int status = 0;
  HttpHeaders headers;
  std::string body;  // full body for unary calls; error body for failed streaming calls
};

/// Receives raw body bytes of a successful streaming response. Return false to abort.
using ChunkCallback = std::function<bool(std::string_view)>;

/// The client's only dependency on the network. Tests substitute a scripted fake.
class Transport {
 public:
  virtual ~Transport() = default;
  [[nodiscard]] virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
  [[nodiscard]] virtual Result<HttpResponse> send_streaming(const HttpRequest& request,
                                                            const ChunkCallback& on_chunk) = 0;
};

/// libcurl-backed transport. Thread-safe: each thread reuses its own easy handle so
/// keep-alive connections survive between calls.
[[nodiscard]] std::unique_ptr<Transport> make_curl_transport();

}  // namespace gemini
