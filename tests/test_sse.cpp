#include "gemini/sse.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using gemini::SseParser;

namespace {
std::vector<std::string> collect(const std::vector<std::string>& pieces, bool finish = true) {
  std::vector<std::string> events;
  SseParser parser([&](std::string_view data) {
    events.emplace_back(data);
    return true;
  });
  for (const auto& p : pieces) parser.feed(p);
  if (finish) parser.finish();
  return events;
}
}  // namespace

TEST(Sse, SingleEvents) {
  auto events = collect({"data: {\"a\":1}\n\ndata: {\"b\":2}\n\n"});
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], "{\"a\":1}");
  EXPECT_EQ(events[1], "{\"b\":2}");
}

TEST(Sse, SplitAcrossArbitraryBoundaries) {
  const std::string stream = "data: hello\r\n\r\ndata: wor\r\ndata: ld\r\n\r\n";
  for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
    auto events = collect({stream.substr(0, cut), stream.substr(cut)});
    ASSERT_EQ(events.size(), 2u) << "cut at " << cut;
    EXPECT_EQ(events[0], "hello");
    EXPECT_EQ(events[1], "wor\nld");
  }
  // byte by byte
  std::vector<std::string> bytes;
  for (char c : stream) bytes.emplace_back(1, c);
  EXPECT_EQ(collect(bytes).size(), 2u);
}

TEST(Sse, IgnoresCommentsAndOtherFields) {
  auto events = collect({": keep-alive\nevent: message\nid: 7\nretry: 100\ndata:no-space\n\n"});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], "no-space");
}

TEST(Sse, BlankLinesWithoutDataDoNotDispatch) { EXPECT_TRUE(collect({"\n\n\n"}).empty()); }

TEST(Sse, FinishFlushesUnterminatedEvent) {
  EXPECT_TRUE(collect({"data: tail"}, false).empty());
  auto events = collect({"data: tail"});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], "tail");
}

TEST(Sse, HandlerCanStop) {
  int calls = 0;
  SseParser parser([&](std::string_view) { return ++calls < 2; });
  EXPECT_FALSE(parser.feed("data: 1\n\ndata: 2\n\ndata: 3\n\n"));
  EXPECT_EQ(calls, 2);
}
