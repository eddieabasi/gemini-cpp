#include "fake_transport.hpp"

#include "gemini/client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace gemini;
using gemini::testing::FakeTransport;
using gemini::testing::text_response;
using std::chrono::milliseconds;

namespace {

struct Harness {
  FakeTransport* transport = nullptr;
  std::vector<milliseconds> sleeps;
  std::unique_ptr<Client> client;

  explicit Harness(RetryPolicy retry = {.max_attempts = 3, .jitter = 0.0}) {
    auto fake = std::make_unique<FakeTransport>();
    transport = fake.get();
    ClientOptions o;
    o.api_key = "test-key";
    o.model = "models/gemini-test";  // prefix is stripped
    o.base_url = "https://example.test/v1beta/";
    o.retry = retry;
    o.sleeper = [this](milliseconds d) { sleeps.push_back(d); };
    client = std::make_unique<Client>(std::move(o), std::move(fake));
  }
};

std::string header(const HttpRequest& r, const std::string& name) {
  for (const auto& [k, v] : r.headers) {
    if (k == name) return v;
  }
  return {};
}

}  // namespace

TEST(Client, GenerateSendsWellFormedRequest) {
  Harness h;
  h.transport->push_json(200, text_response("pong"));
  auto r = h.client->generate("ping");
  ASSERT_TRUE(r.has_value()) << r.error().describe();
  EXPECT_EQ(r->text(), "pong");

  ASSERT_EQ(h.transport->requests.size(), 1u);
  const auto& req = h.transport->requests[0];
  EXPECT_EQ(req.url, "https://example.test/v1beta/models/gemini-test:generateContent");
  EXPECT_EQ(req.method, "POST");
  EXPECT_EQ(header(req, "x-goog-api-key"), "test-key");
  EXPECT_EQ(header(req, "Content-Type"), "application/json");
  EXPECT_EQ(req.url.find("key="), std::string::npos) << "API key must not leak into URLs";
  const auto body = nlohmann::json::parse(req.body);
  EXPECT_EQ(body["contents"][0]["parts"][0]["text"], "ping");

  const auto m = h.client->metrics();
  EXPECT_EQ(m.requests, 1u);
  EXPECT_EQ(m.prompt_tokens, 10u);
  EXPECT_EQ(m.output_tokens, 5u);
}

TEST(Client, RetriesTransientErrorsWithBackoff) {
  Harness h({.max_attempts = 4, .initial_backoff = milliseconds(100), .jitter = 0.0});
  h.transport->push_json(503, R"({"error":{"message":"overloaded","status":"UNAVAILABLE"}})");
  h.transport->push_error(ErrorCode::kTransport, "connection reset");
  h.transport->push_json(200, text_response("ok"));
  auto r = h.client->generate("x");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(h.transport->requests.size(), 3u);
  ASSERT_EQ(h.sleeps.size(), 2u);
  EXPECT_EQ(h.sleeps[0], milliseconds(100));
  EXPECT_EQ(h.sleeps[1], milliseconds(200));
  EXPECT_EQ(h.client->metrics().retries, 2u);
}

TEST(Client, HonoursServerRetryDelay) {
  Harness h;
  h.transport->push_json(429, R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED","message":"quota",
      "details":[{"@type":"type.googleapis.com/google.rpc.RetryInfo","retryDelay":"2s"}]}})");
  h.transport->push_json(200, text_response("ok"));
  ASSERT_TRUE(h.client->generate("x").has_value());
  ASSERT_EQ(h.sleeps.size(), 1u);
  EXPECT_EQ(h.sleeps[0], milliseconds(2000));
}

TEST(Client, GivesUpAfterMaxAttempts) {
  Harness h({.max_attempts = 2, .jitter = 0.0});
  for (int i = 0; i < 5; ++i) h.transport->push_json(500, "{}");
  auto r = h.client->generate("x");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kServer);
  EXPECT_EQ(h.transport->requests.size(), 2u);
  EXPECT_EQ(h.client->metrics().failures, 1u);
}

TEST(Client, DoesNotRetryClientErrors) {
  Harness h;
  h.transport->push_json(400, R"({"error":{"message":"API key not valid","status":"INVALID_ARGUMENT"}})");
  auto r = h.client->generate("x");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(r.error().message, "API key not valid");
  EXPECT_EQ(h.transport->requests.size(), 1u);
  EXPECT_TRUE(h.sleeps.empty());
}

TEST(Client, BlockedPromptIsAnError) {
  Harness h;
  h.transport->push_json(200, R"({"promptFeedback":{"blockReason":"SAFETY"}})");
  auto r = h.client->generate("x");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kBlocked);
}

TEST(Client, ValidatesConfiguration) {
  ClientOptions o;  // no key
  Client c(std::move(o), std::make_unique<FakeTransport>());
  auto r = c.generate("x");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kConfig);

  Harness h;
  auto empty = h.client->generate(GenerateRequest{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().code, ErrorCode::kInvalidArgument);
  EXPECT_TRUE(h.transport->requests.empty());
}

TEST(Client, StreamAggregatesChunksAndDeliversThemInOrder) {
  Harness h;
  h.transport->push_stream({
      "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Hel\"}]}}]}\r\n\r\n",
      "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"lo\"}]},",
      "\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":1,\"totalTokenCount\":3}}\r\n\r\n"});
  std::vector<std::string> seen;
  auto r = h.client->stream(GenerateRequest{{Content::user("hi")}, {}, {}, {}, {}}, [&](const GenerateResponse& c) {
    seen.push_back(c.text());
    return true;
  });
  ASSERT_TRUE(r.has_value()) << r.error().describe();
  EXPECT_EQ(r->text(), "Hello");
  EXPECT_EQ(r->finish_reason(), "STOP");
  EXPECT_EQ(r->usage.total_tokens, 3);
  EXPECT_EQ(seen, (std::vector<std::string>{"Hel", "lo"}));
  EXPECT_NE(h.transport->requests[0].url.find(":streamGenerateContent?alt=sse"), std::string::npos);
}

TEST(Client, StreamRetriesOnlyBeforeFirstChunk) {
  {  // failure before anything was delivered: retried
    Harness h;
    h.transport->push_json(503, "{}");
    h.transport->push_stream({"data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"ok\"}]}}]}\n\n"});
    auto r = h.client->stream(GenerateRequest{{Content::user("hi")}, {}, {}, {}, {}}, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text(), "ok");
    EXPECT_EQ(h.transport->requests.size(), 2u);
  }
  {  // failure mid-stream: not retried, so the caller never sees duplicated text
    Harness h;
    FakeTransport::Reply broken;
    broken.chunks = {"data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"partial\"}]}}]}\n\n"};
    broken.error = Error{ErrorCode::kTransport, "connection reset", 0, {}, std::nullopt};
    h.transport->push(broken);
    h.transport->push_stream({"data: {}\n\n"});
    auto r = h.client->stream(GenerateRequest{{Content::user("hi")}, {}, {}, {}, {}}, nullptr);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::kTransport);
    EXPECT_EQ(h.transport->requests.size(), 1u);
  }
}

TEST(Client, StreamCancellationReturnsPartialResult) {
  Harness h;
  h.transport->push_stream({"data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"a\"}]}}]}\n\n",
                            "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"b\"}]}}]}\n\n"});
  auto r = h.client->stream(GenerateRequest{{Content::user("hi")}, {}, {}, {}, {}},
                            [](const GenerateResponse&) { return false; });
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->text(), "a");
}

TEST(Client, StreamHttpErrorBodyIsParsed) {
  Harness h({.max_attempts = 1});
  FakeTransport::Reply reply;
  reply.status = 404;
  reply.body = R"([{"error":{"code":404,"message":"models/nope is not found","status":"NOT_FOUND"}}])";
  h.transport->push(reply);
  auto r = h.client->stream(GenerateRequest{{Content::user("hi")}, {}, {}, {}, {}}, nullptr);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kNotFound);
  EXPECT_EQ(r.error().message, "models/nope is not found");
}

TEST(Client, CountTokens) {
  Harness h;
  h.transport->push_json(200, R"({"totalTokens": 42})");
  GenerateRequest req{{Content::user("count me")}, Content::user("sys"), {}, {}, {}};
  auto n = h.client->count_tokens(req);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 42);
  const auto body = nlohmann::json::parse(h.transport->requests[0].body);
  EXPECT_EQ(body["generateContentRequest"]["model"], "models/gemini-test");
  EXPECT_EQ(body["generateContentRequest"]["systemInstruction"]["parts"][0]["text"], "sys");
}

TEST(Client, ListModelsFollowsPagination) {
  Harness h;
  h.transport->push_json(200, R"({"models":[{"name":"models/a","supportedGenerationMethods":["generateContent"]}],
                                  "nextPageToken":"p2"})");
  h.transport->push_json(200, R"({"models":[{"name":"models/b","inputTokenLimit":1048576}]})");
  auto models = h.client->list_models();
  ASSERT_TRUE(models.has_value());
  ASSERT_EQ(models->size(), 2u);
  EXPECT_EQ((*models)[0].supported_methods[0], "generateContent");
  EXPECT_EQ((*models)[1].input_token_limit, 1048576);
  EXPECT_EQ(h.transport->requests[0].method, "GET");
  EXPECT_NE(h.transport->requests[1].url.find("pageToken=p2"), std::string::npos);
}

TEST(Client, ConcurrentCallsAreSafe) {
  Harness h;
  constexpr int kCalls = 64;
  for (int i = 0; i < kCalls; ++i) h.transport->push_json(200, text_response("ok"));
  std::vector<std::thread> threads;
  std::atomic<int> ok{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kCalls / 8; ++i) ok += h.client->generate("x").has_value() ? 1 : 0;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(ok.load(), kCalls);
  EXPECT_EQ(h.client->metrics().requests, static_cast<std::uint64_t>(kCalls));
}
