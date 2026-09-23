#include "gemini/serialization.hpp"

#include <gtest/gtest.h>

using namespace gemini;
using nlohmann::json;

TEST(Serialization, MinimalRequestOmitsEmptySections) {
  GenerateRequest req;
  req.contents.push_back(Content::user("hi"));
  const json j = wire::to_json(req);
  EXPECT_EQ(j["contents"][0]["role"], "user");
  EXPECT_EQ(j["contents"][0]["parts"][0]["text"], "hi");
  EXPECT_FALSE(j.contains("generationConfig"));
  EXPECT_FALSE(j.contains("tools"));
  EXPECT_FALSE(j.contains("systemInstruction"));
  EXPECT_FALSE(j.contains("safetySettings"));
}

TEST(Serialization, FullRequest) {
  GenerateRequest req;
  req.contents.push_back(Content::user("q"));
  req.system_instruction = Content::user("be terse");
  req.config.temperature = 0.25;
  req.config.max_output_tokens = 64;
  req.config.thinking_budget = 0;
  req.config.response_mime_type = "application/json";
  req.config.stop_sequences = {"END"};
  req.tools.push_back({"lookup", "Looks things up", {{"type", "object"}}});
  req.safety_settings.push_back({"HARM_CATEGORY_HARASSMENT", "BLOCK_ONLY_HIGH"});

  const json j = wire::to_json(req);
  EXPECT_EQ(j["systemInstruction"]["parts"][0]["text"], "be terse");
  EXPECT_DOUBLE_EQ(j["generationConfig"]["temperature"].get<double>(), 0.25);
  EXPECT_EQ(j["generationConfig"]["maxOutputTokens"], 64);
  EXPECT_EQ(j["generationConfig"]["thinkingConfig"]["thinkingBudget"], 0);
  EXPECT_EQ(j["generationConfig"]["responseMimeType"], "application/json");
  EXPECT_EQ(j["generationConfig"]["stopSequences"][0], "END");
  EXPECT_EQ(j["tools"][0]["functionDeclarations"][0]["name"], "lookup");
  EXPECT_EQ(j["safetySettings"][0]["threshold"], "BLOCK_ONLY_HIGH");
}

TEST(Serialization, FunctionPartsAndThoughtSignatureRoundTrip) {
  Part call = Part::call({"get_weather", {{"city", "Lagos"}}, "call-1"});
  call.thought_signature = "c2lnbmF0dXJl";
  const json j = wire::to_json(call);
  EXPECT_EQ(j["functionCall"]["name"], "get_weather");
  EXPECT_EQ(j["functionCall"]["args"]["city"], "Lagos");
  EXPECT_EQ(j["thoughtSignature"], "c2lnbmF0dXJl");

  auto parsed = wire::parse_part(j);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_NE(parsed->as_call(), nullptr);
  EXPECT_EQ(parsed->as_call()->id, "call-1");
  EXPECT_EQ(parsed->thought_signature, "c2lnbmF0dXJl");

  const json r = wire::to_json(Part::response({"get_weather", {{"temp_c", 31}}, ""}));
  EXPECT_EQ(r["functionResponse"]["response"]["temp_c"], 31);
}

TEST(Serialization, ParsesTextThoughtsUsageAndSkipsUnknownParts) {
  const std::string body = R"({
    "candidates": [{
      "content": {"role": "model", "parts": [
        {"text": "thinking...", "thought": true},
        {"inlineData": {"mimeType": "image/png", "data": "AAAA"}},
        {"text": "Hello"}, {"text": ", world"}
      ]},
      "finishReason": "STOP", "index": 0
    }],
    "usageMetadata": {"promptTokenCount": 3, "candidatesTokenCount": 4, "thoughtsTokenCount": 7, "totalTokenCount": 14},
    "modelVersion": "gemini-2.5-flash", "responseId": "abc"
  })";
  auto r = wire::parse_generate_response(body);
  ASSERT_TRUE(r.has_value()) << r.error().describe();
  EXPECT_EQ(r->text(), "Hello, world");  // thought excluded
  EXPECT_EQ(r->candidates[0].content.parts.size(), 3u);
  EXPECT_EQ(r->finish_reason(), "STOP");
  EXPECT_EQ(r->usage.thoughts_tokens, 7);
  EXPECT_EQ(r->usage.total_tokens, 14);
  EXPECT_EQ(r->model_version, "gemini-2.5-flash");
  EXPECT_EQ(r->response_id, "abc");
}

TEST(Serialization, FunctionCallsExtracted) {
  auto r = wire::parse_generate_response(
      R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"a","args":{"x":1}}},{"functionCall":{"name":"b"}}]}}]})");
  ASSERT_TRUE(r.has_value());
  const auto calls = r->function_calls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].name, "a");
  EXPECT_EQ(calls[0].args["x"], 1);
  EXPECT_EQ(calls[1].name, "b");
}

TEST(Serialization, PromptBlocked) {
  auto r = wire::parse_generate_response(R"({"promptFeedback":{"blockReason":"SAFETY"}})");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->candidates.empty());
  EXPECT_EQ(r->prompt_block_reason, "SAFETY");
}

TEST(Serialization, MalformedJsonIsParseError) {
  auto r = wire::parse_generate_response("{not json");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kParse);
  auto bad_call = wire::parse_generate_response(R"({"candidates":[{"content":{"parts":[{"functionCall":{}}]}}]})");
  ASSERT_FALSE(bad_call.has_value());
}

TEST(Serialization, ApiErrorClassification) {
  const auto e = wire::parse_api_error(429, R"({"error":{"code":429,"message":"Quota exceeded","status":"RESOURCE_EXHAUSTED",
      "details":[{"@type":"type.googleapis.com/google.rpc.RetryInfo","retryDelay":"12.5s"}]}})");
  EXPECT_EQ(e.code, ErrorCode::kRateLimited);
  EXPECT_EQ(e.message, "Quota exceeded");
  EXPECT_EQ(e.api_status, "RESOURCE_EXHAUSTED");
  ASSERT_TRUE(e.retry_after.has_value());
  EXPECT_EQ(e.retry_after->count(), 12500);
  EXPECT_TRUE(e.retryable());

  EXPECT_EQ(wire::parse_api_error(400, R"({"error":{"message":"bad","status":"INVALID_ARGUMENT"}})").code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(wire::parse_api_error(403, "{}").code, ErrorCode::kUnauthenticated);
  EXPECT_EQ(wire::parse_api_error(404, "").code, ErrorCode::kNotFound);
  EXPECT_EQ(wire::parse_api_error(503, "<html>oops</html>").code, ErrorCode::kServer);
  EXPECT_NE(wire::parse_api_error(503, "<html>oops</html>").message.find("oops"), std::string::npos);
  EXPECT_FALSE(wire::parse_api_error(400, "{}").retryable());

  // Streaming endpoints wrap the error in an array; Retry-After header is a fallback.
  const auto streamed = wire::parse_api_error(500, R"([{"error":{"message":"internal","status":"INTERNAL"}}])",
                                              {{"retry-after", "3"}});
  EXPECT_EQ(streamed.message, "internal");
  EXPECT_EQ(streamed.retry_after, std::chrono::milliseconds(3000));
}

TEST(Serialization, ProtoDuration) {
  EXPECT_EQ(wire::parse_proto_duration("30s"), std::chrono::milliseconds(30000));
  EXPECT_EQ(wire::parse_proto_duration("0.001s"), std::chrono::milliseconds(1));
  EXPECT_FALSE(wire::parse_proto_duration("30").has_value());
  EXPECT_FALSE(wire::parse_proto_duration("xs").has_value());
  EXPECT_FALSE(wire::parse_proto_duration("-1s").has_value());
}

TEST(Types, MergeStreamChunks) {
  GenerateResponse agg;
  auto c1 = wire::parse_generate_response(R"({"candidates":[{"content":{"parts":[{"text":"Hel"}]}}]})");
  auto c2 = wire::parse_generate_response(
      R"({"candidates":[{"content":{"parts":[{"text":"lo"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":2,"totalTokenCount":5}})");
  ASSERT_TRUE(c1 && c2);
  agg.merge(*c1);
  agg.merge(*c2);
  EXPECT_EQ(agg.text(), "Hello");
  EXPECT_EQ(agg.candidates[0].content.parts.size(), 1u);
  EXPECT_EQ(agg.finish_reason(), "STOP");
  EXPECT_EQ(agg.usage.total_tokens, 5);
}

TEST(Types, MergeKeepsThoughtsAndCallsSeparate) {
  GenerateResponse agg;
  GenerateResponse chunk;
  Part thought = Part::text("hmm");
  thought.thought = true;
  chunk.candidates.push_back({Content{Role::kModel, {thought, Part::text("A")}}, "", 0});
  agg.merge(chunk);
  GenerateResponse chunk2;
  chunk2.candidates.push_back({Content{Role::kModel, {Part::call({"f", json::object(), ""}), Part::text("B")}}, "", 0});
  agg.merge(chunk2);
  const auto& parts = agg.candidates[0].content.parts;
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_TRUE(parts[0].thought);
  EXPECT_EQ(agg.text(), "AB");
  EXPECT_EQ(agg.function_calls().size(), 1u);
}

TEST(Errors, Describe) {
  Error e{ErrorCode::kRateLimited, "slow down", 429, "RESOURCE_EXHAUSTED", std::nullopt};
  EXPECT_EQ(e.describe(), "[rate_limited] slow down (HTTP 429 RESOURCE_EXHAUSTED)");
  EXPECT_EQ((Error{ErrorCode::kConfig, "no key", 0, {}, std::nullopt}.describe()), "[config] no key");
}
