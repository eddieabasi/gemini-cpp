#include "gemini/client.hpp"

#include "gemini/logging.hpp"
#include "gemini/serialization.hpp"
#include "gemini/sse.hpp"

#include <cstdlib>
#include <thread>

namespace gemini {

namespace {

std::string env_or(const char* name, std::string fallback = {}) {
  const char* value = std::getenv(name);
  return (value != nullptr && *value != '\0') ? std::string(value) : std::move(fallback);
}

std::int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

Status check_http(const Result<HttpResponse>& response) {
  if (!response) return std::unexpected(response.error());
  if (response->status < 200 || response->status >= 300) {
    return std::unexpected(wire::parse_api_error(response->status, response->body, response->headers));
  }
  return {};
}

Result<GenerateResponse> check_blocked(Result<GenerateResponse> result) {
  if (result && result->candidates.empty() && result->prompt_block_reason) {
    return make_error(ErrorCode::kBlocked, "prompt blocked: " + *result->prompt_block_reason);
  }
  return result;
}

}  // namespace

Result<ClientOptions> ClientOptions::from_env() {
  ClientOptions options;
  options.api_key = env_or("GEMINI_API_KEY", env_or("GOOGLE_API_KEY"));
  if (options.api_key.empty()) {
    return make_error(ErrorCode::kConfig,
                      "no API key: set GEMINI_API_KEY (get one at https://aistudio.google.com/apikey)");
  }
  options.model = env_or("GEMINI_MODEL", std::string(kDefaultModel));
  options.base_url = env_or("GEMINI_BASE_URL", std::string(kDefaultBaseUrl));
  return options;
}

Client::Client(ClientOptions options) : Client(std::move(options), make_curl_transport()) {}

Client::Client(ClientOptions options, std::unique_ptr<Transport> transport)
    : options_(std::move(options)), transport_(std::move(transport)) {
  if (options_.model.starts_with("models/")) options_.model.erase(0, 7);
  while (options_.base_url.ends_with('/')) options_.base_url.pop_back();
  if (!options_.sleeper) {
    options_.sleeper = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
  }
}

Client::~Client() = default;

std::string Client::endpoint(std::string_view method, bool sse) const {
  std::string url = options_.base_url + "/models/" + options_.model + ":" + std::string(method);
  if (sse) url += "?alt=sse";
  return url;
}

HttpRequest Client::make_request(std::string url, std::string body) const {
  HttpRequest req;
  req.url = std::move(url);
  req.body = std::move(body);
  req.timeout = options_.timeout;
  req.headers = {{"Content-Type", "application/json"}, {"x-goog-api-key", options_.api_key}};
  return req;
}

Status Client::validate(const GenerateRequest& request) const {
  if (options_.api_key.empty()) return make_error(ErrorCode::kConfig, "API key is empty");
  if (options_.model.empty()) return make_error(ErrorCode::kConfig, "model name is empty");
  if (request.contents.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "request has no contents");
  }
  return {};
}

template <typename T, typename Attempt, typename CanRetry>
Result<T> Client::with_retry(std::string_view op, Attempt&& attempt, CanRetry&& can_retry) {
  const int max_attempts = std::max(1, options_.retry.max_attempts);
  for (int i = 0;; ++i) {
    metrics_.attempts.fetch_add(1, std::memory_order_relaxed);
    Result<T> result = attempt();
    if (result || !result.error().retryable() || i + 1 >= max_attempts || !can_retry()) {
      return result;
    }
    const auto delay = options_.retry.delay_for(i, result.error());
    log::warn("gemini.retry", {{"op", std::string(op)},
                               {"attempt", std::to_string(i + 1)},
                               {"delay_ms", std::to_string(delay.count())},
                               {"error", result.error().describe()}});
    metrics_.retries.fetch_add(1, std::memory_order_relaxed);
    options_.sleeper(delay);
  }
}

void Client::record(std::string_view op, std::chrono::steady_clock::time_point start,
                    const Result<GenerateResponse>& result) {
  const auto ms = elapsed_ms(start);
  metrics_.total_latency_ms.fetch_add(static_cast<std::uint64_t>(ms), std::memory_order_relaxed);
  if (result) {
    metrics_.prompt_tokens.fetch_add(static_cast<std::uint64_t>(result->usage.prompt_tokens),
                                     std::memory_order_relaxed);
    metrics_.output_tokens.fetch_add(
        static_cast<std::uint64_t>(result->usage.candidates_tokens + result->usage.thoughts_tokens),
        std::memory_order_relaxed);
    log::info("gemini.response", {{"op", std::string(op)},
                                  {"model", options_.model},
                                  {"latency_ms", std::to_string(ms)},
                                  {"finish", result->finish_reason()},
                                  {"prompt_tokens", std::to_string(result->usage.prompt_tokens)},
                                  {"output_tokens", std::to_string(result->usage.candidates_tokens)}});
  } else {
    metrics_.failures.fetch_add(1, std::memory_order_relaxed);
    log::error("gemini.failure", {{"op", std::string(op)},
                                  {"latency_ms", std::to_string(ms)},
                                  {"error", result.error().describe()}});
  }
}

Result<GenerateResponse> Client::generate(std::string_view prompt) {
  GenerateRequest request;
  request.contents.push_back(Content::user(std::string(prompt)));
  return generate(request);
}

Result<GenerateResponse> Client::generate(const GenerateRequest& request) {
  metrics_.requests.fetch_add(1, std::memory_order_relaxed);
  if (auto ok = validate(request); !ok) return std::unexpected(ok.error());

  const auto start = std::chrono::steady_clock::now();
  const std::string body = wire::to_json(request).dump();
  auto result = with_retry<GenerateResponse>(
      "generate",
      [&]() -> Result<GenerateResponse> {
        auto response = transport_->send(make_request(endpoint("generateContent"), body));
        if (auto ok = check_http(response); !ok) return std::unexpected(ok.error());
        return wire::parse_generate_response(response->body);
      },
      [] { return true; });
  result = check_blocked(std::move(result));
  record("generate", start, result);
  return result;
}

Result<GenerateResponse> Client::stream(const GenerateRequest& request,
                                        const StreamCallback& on_chunk) {
  metrics_.requests.fetch_add(1, std::memory_order_relaxed);
  if (auto ok = validate(request); !ok) return std::unexpected(ok.error());

  const auto start = std::chrono::steady_clock::now();
  const std::string body = wire::to_json(request).dump();
  bool delivered_any = false;

  auto result = with_retry<GenerateResponse>(
      "stream",
      [&]() -> Result<GenerateResponse> {
        GenerateResponse aggregate;
        std::optional<Error> parse_error;
        bool caller_stopped = false;

        SseParser parser([&](std::string_view data) {
          if (data == "[DONE]") return true;
          auto chunk = wire::parse_generate_response(data);
          if (!chunk) {
            parse_error = chunk.error();
            return false;
          }
          aggregate.merge(*chunk);
          delivered_any = true;
          if (on_chunk && !on_chunk(*chunk)) {
            caller_stopped = true;
            return false;
          }
          return true;
        });

        auto response = transport_->send_streaming(
            make_request(endpoint("streamGenerateContent", true), body),
            [&](std::string_view bytes) { return parser.feed(bytes); });

        if (parse_error) return std::unexpected(*parse_error);
        if (caller_stopped) return aggregate;  // partial result is what the caller asked for
        if (auto ok = check_http(response); !ok) return std::unexpected(ok.error());
        parser.finish();
        if (parse_error) return std::unexpected(*parse_error);
        return aggregate;
      },
      [&] { return !delivered_any; });

  result = check_blocked(std::move(result));
  record("stream", start, result);
  return result;
}

Result<std::int64_t> Client::count_tokens(const GenerateRequest& request) {
  metrics_.requests.fetch_add(1, std::memory_order_relaxed);
  if (auto ok = validate(request); !ok) return std::unexpected(ok.error());

  nlohmann::json inner = wire::to_json(request);
  inner["model"] = "models/" + options_.model;
  const std::string body = nlohmann::json{{"generateContentRequest", std::move(inner)}}.dump();

  auto result = with_retry<std::int64_t>(
      "count_tokens",
      [&]() -> Result<std::int64_t> {
        auto response = transport_->send(make_request(endpoint("countTokens"), body));
        if (auto ok = check_http(response); !ok) return std::unexpected(ok.error());
        auto j = nlohmann::json::parse(response->body, nullptr, false);
        if (j.is_discarded() || !j.contains("totalTokens") || !j["totalTokens"].is_number_integer()) {
          return make_error(ErrorCode::kParse, "countTokens response missing totalTokens");
        }
        return j["totalTokens"].get<std::int64_t>();
      },
      [] { return true; });
  if (!result) metrics_.failures.fetch_add(1, std::memory_order_relaxed);
  return result;
}

Result<std::vector<ModelInfo>> Client::list_models() {
  metrics_.requests.fetch_add(1, std::memory_order_relaxed);
  if (options_.api_key.empty()) return make_error(ErrorCode::kConfig, "API key is empty");

  std::vector<ModelInfo> all;
  std::string page_token;
  do {
    std::string url = options_.base_url + "/models?pageSize=100";
    if (!page_token.empty()) url += "&pageToken=" + page_token;
    auto page = with_retry<std::vector<ModelInfo>>(
        "list_models",
        [&]() -> Result<std::vector<ModelInfo>> {
          HttpRequest req = make_request(url, {});
          req.method = "GET";
          auto response = transport_->send(req);
          if (auto ok = check_http(response); !ok) return std::unexpected(ok.error());
          return wire::parse_model_list(response->body, &page_token);
        },
        [] { return true; });
    if (!page) {
      metrics_.failures.fetch_add(1, std::memory_order_relaxed);
      return page;
    }
    all.insert(all.end(), std::make_move_iterator(page->begin()), std::make_move_iterator(page->end()));
  } while (!page_token.empty());
  return all;
}

MetricsSnapshot Client::metrics() const noexcept {
  MetricsSnapshot s;
  s.requests = metrics_.requests.load();
  s.attempts = metrics_.attempts.load();
  s.retries = metrics_.retries.load();
  s.failures = metrics_.failures.load();
  s.prompt_tokens = metrics_.prompt_tokens.load();
  s.output_tokens = metrics_.output_tokens.load();
  s.total_latency_ms = metrics_.total_latency_ms.load();
  return s;
}

}  // namespace gemini
