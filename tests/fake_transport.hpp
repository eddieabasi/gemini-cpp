#pragma once

#include "gemini/transport.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gemini::testing {

/// Scripted transport: each call pops the next scripted reply and records the request.
class FakeTransport final : public Transport {
 public:
  struct Reply {
    int status = 200;
    std::string body;                 // unary body, or error body for streaming
    std::vector<std::string> chunks;  // streaming body, delivered piece by piece
    std::optional<Error> error;       // transport-level failure
    HttpHeaders headers;
  };

  void push(Reply reply) { replies_.push_back(std::move(reply)); }
  void push_json(int status, std::string body) { push(Reply{status, std::move(body), {}, std::nullopt, {}}); }
  void push_stream(std::vector<std::string> chunks) {
    push(Reply{200, {}, std::move(chunks), std::nullopt, {}});
  }
  void push_error(ErrorCode code, std::string message) {
    push(Reply{0, {}, {}, Error{code, std::move(message), 0, {}, std::nullopt}, {}});
  }

  Result<HttpResponse> send(const HttpRequest& request) override {
    auto reply = next(request);
    if (reply.error) return std::unexpected(*reply.error);
    return HttpResponse{reply.status, reply.headers, reply.body};
  }

  Result<HttpResponse> send_streaming(const HttpRequest& request, const ChunkCallback& on_chunk) override {
    auto reply = next(request);
    if (reply.status >= 200 && reply.status < 300) {
      for (const auto& chunk : reply.chunks) {
        if (!on_chunk(chunk)) return make_error(ErrorCode::kCancelled, "cancelled");
      }
    }
    if (reply.error) return std::unexpected(*reply.error);
    return HttpResponse{reply.status, reply.headers, reply.body};
  }

  std::vector<HttpRequest> requests;

 private:
  Reply next(const HttpRequest& request) {
    std::lock_guard lock(mutex_);
    requests.push_back(request);
    if (replies_.empty()) return Reply{500, R"({"error":{"message":"no scripted reply"}})", {}, std::nullopt, {}};
    Reply r = std::move(replies_.front());
    replies_.pop_front();
    return r;
  }

  std::mutex mutex_;
  std::deque<Reply> replies_;
};

inline std::string text_response(const std::string& text, const std::string& finish = "STOP") {
  nlohmann::json j = {{"candidates",
                       {{{"content", {{"role", "model"}, {"parts", {{{"text", text}}}}}},
                         {"finishReason", finish},
                         {"index", 0}}}},
                      {"usageMetadata", {{"promptTokenCount", 10}, {"candidatesTokenCount", 5}, {"totalTokenCount", 15}}},
                      {"modelVersion", "gemini-test"}};
  return j.dump();
}

}  // namespace gemini::testing
