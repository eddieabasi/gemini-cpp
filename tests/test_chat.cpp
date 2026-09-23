#include "fake_transport.hpp"

#include "gemini/chat.hpp"

#include <gtest/gtest.h>

using namespace gemini;
using gemini::testing::FakeTransport;
using gemini::testing::text_response;
using nlohmann::json;

namespace {

std::unique_ptr<Client> make_client(FakeTransport*& out) {
  auto fake = std::make_unique<FakeTransport>();
  out = fake.get();
  ClientOptions o;
  o.api_key = "k";
  o.retry = RetryPolicy::none();
  return std::make_unique<Client>(std::move(o), std::move(fake));
}

}  // namespace

TEST(Chat, KeepsHistoryAcrossTurns) {
  FakeTransport* t = nullptr;
  auto client = make_client(t);
  t->push_json(200, text_response("Hi Ada"));
  t->push_json(200, text_response("Your name is Ada"));

  ChatSession chat(*client, {.system_instruction = "be nice"});
  ASSERT_TRUE(chat.send("I am Ada").has_value());
  auto r = chat.send("What is my name?");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->text(), "Your name is Ada");
  EXPECT_EQ(chat.history().size(), 4u);
  EXPECT_EQ(chat.usage().total_tokens, 30);

  const auto second = json::parse(t->requests[1].body);
  ASSERT_EQ(second["contents"].size(), 3u);
  EXPECT_EQ(second["contents"][1]["role"], "model");
  EXPECT_EQ(second["contents"][1]["parts"][0]["text"], "Hi Ada");
  EXPECT_EQ(second["systemInstruction"]["parts"][0]["text"], "be nice");
}

TEST(Chat, RunsToolsAndEchoesThoughtSignatures) {
  FakeTransport* t = nullptr;
  auto client = make_client(t);
  t->push_json(200, R"({"candidates":[{"content":{"role":"model","parts":[
      {"functionCall":{"name":"evaluate_expression","args":{"expression":"6*7"}},"thoughtSignature":"SIG"}]}}]})");
  t->push_json(200, text_response("The answer is 42."));

  ToolRegistry tools;
  builtin_tools::register_defaults(tools);
  ChatSession chat(*client, {}, &tools);
  std::vector<std::string> observed;
  chat.set_tool_observer([&](const FunctionCall& c, const FunctionResponse& r) {
    observed.push_back(c.name + "=" + r.response.dump());
  });

  auto r = chat.send("what is 6*7?");
  ASSERT_TRUE(r.has_value()) << r.error().describe();
  EXPECT_EQ(r->text(), "The answer is 42.");
  ASSERT_EQ(observed.size(), 1u);
  EXPECT_EQ(observed[0], R"(evaluate_expression={"value":42.0})");

  const auto first = json::parse(t->requests[0].body);
  EXPECT_EQ(first["tools"][0]["functionDeclarations"].size(), 2u);

  const auto second = json::parse(t->requests[1].body);
  const auto& contents = second["contents"];
  ASSERT_EQ(contents.size(), 3u);
  EXPECT_EQ(contents[1]["parts"][0]["thoughtSignature"], "SIG");
  EXPECT_EQ(contents[2]["role"], "user");
  EXPECT_EQ(contents[2]["parts"][0]["functionResponse"]["name"], "evaluate_expression");
  EXPECT_DOUBLE_EQ(contents[2]["parts"][0]["functionResponse"]["response"]["value"].get<double>(), 42.0);
}

TEST(Chat, RollsBackHistoryOnError) {
  FakeTransport* t = nullptr;
  auto client = make_client(t);
  t->push_json(200, text_response("first"));
  t->push_json(400, R"({"error":{"message":"bad"}})");
  ChatSession chat(*client);
  ASSERT_TRUE(chat.send("one").has_value());
  EXPECT_FALSE(chat.send("two").has_value());
  EXPECT_EQ(chat.history().size(), 2u);
}

TEST(Chat, StopsRunawayToolLoops) {
  FakeTransport* t = nullptr;
  auto client = make_client(t);
  const std::string call = R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"get_current_time"}}]}}]})";
  for (int i = 0; i < 10; ++i) t->push_json(200, call);
  ToolRegistry tools;
  builtin_tools::register_defaults(tools);
  ChatSession chat(*client, {.max_tool_rounds = 2}, &tools);
  auto r = chat.send("loop forever");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(t->requests.size(), 3u);
  EXPECT_TRUE(chat.history().empty());
}
