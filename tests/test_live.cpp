// Smoke tests against the real Gemini API. Skipped unless GEMINI_API_KEY is set, so CI
// stays hermetic; run locally with `GEMINI_API_KEY=... ctest -R Live`.
#include "gemini/chat.hpp"
#include "gemini/client.hpp"

#include <gtest/gtest.h>

using namespace gemini;

namespace {
std::unique_ptr<Client> live_client() {
  auto options = ClientOptions::from_env();
  if (!options) return nullptr;
  return std::make_unique<Client>(std::move(*options));
}
}  // namespace

TEST(Live, GenerateAndStream) {
  auto client = live_client();
  if (!client) GTEST_SKIP() << "GEMINI_API_KEY not set";

  GenerateRequest req;
  req.contents.push_back(Content::user("Reply with exactly the word: pong"));
  req.config.temperature = 0.0;
  auto r = client->generate(req);
  ASSERT_TRUE(r.has_value()) << r.error().describe();
  EXPECT_NE(r->text().find("pong"), std::string::npos) << r->text();
  EXPECT_GT(r->usage.prompt_tokens, 0);

  int chunks = 0;
  auto s = client->stream(req, [&](const GenerateResponse&) { return ++chunks > 0; });
  ASSERT_TRUE(s.has_value()) << s.error().describe();
  EXPECT_GE(chunks, 1);
  EXPECT_NE(s->text().find("pong"), std::string::npos);

  auto n = client->count_tokens(req);
  ASSERT_TRUE(n.has_value()) << n.error().describe();
  EXPECT_GT(*n, 0);
}

TEST(Live, FunctionCalling) {
  auto client = live_client();
  if (!client) GTEST_SKIP() << "GEMINI_API_KEY not set";
  ToolRegistry tools;
  builtin_tools::register_defaults(tools);
  bool called = false;
  ChatSession chat(*client, {}, &tools);
  chat.set_tool_observer([&](const FunctionCall& c, const FunctionResponse&) {
    called = called || c.name == "evaluate_expression";
  });
  auto r = chat.send("Use the evaluate_expression tool to compute 12345 * 6789, then tell me the result.");
  ASSERT_TRUE(r.has_value()) << r.error().describe();
  EXPECT_TRUE(called);
  const std::string text = r->text();
  EXPECT_TRUE(text.find("83810205") != std::string::npos || text.find("83,810,205") != std::string::npos) << text;
}
