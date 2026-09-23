#include "gemini/eval/code_extract.hpp"

#include <gtest/gtest.h>

using namespace gemini::eval;

TEST(CodeExtract, PrefersLongestCppBlock) {
  const std::string reply =
      "Here you go:\n\n```bash\ng++ main.cpp\n```\n\n```cpp\n#pragma once\nint f();\n```\n\n"
      "And a longer one:\n```c++\n#pragma once\nint f() { return 42; }\nint g() { return 1; }\n```\n";
  auto src = extract_cpp_source(reply);
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(*src, "#pragma once\nint f() { return 42; }\nint g() { return 1; }\n");
}

TEST(CodeExtract, FallsBackToUntaggedBlock) {
  auto src = extract_cpp_source("```\nint x = 1;\n```");
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(*src, "int x = 1;\n");
}

TEST(CodeExtract, IgnoresNonCppTaggedBlocks) {
  EXPECT_FALSE(extract_cpp_source("```python\nprint(1)\n```").has_value());
}

TEST(CodeExtract, HandlesTildesLongFencesAndInfoStrings) {
  auto blocks = find_code_blocks("~~~ CPP title=\"x\"\na\n~~~\n````cpp\n```\nnested\n```\n````\n");
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_EQ(blocks[0].language, "cpp");
  EXPECT_EQ(blocks[0].code, "a\n");
  EXPECT_EQ(blocks[1].code, "```\nnested\n```\n");
}

TEST(CodeExtract, KeepsTruncatedBlock) {
  auto blocks = find_code_blocks("```cpp\n#pragma once\nint f() {\n");
  ASSERT_EQ(blocks.size(), 1u);
  EXPECT_FALSE(blocks[0].terminated);
  EXPECT_TRUE(extract_cpp_source("```cpp\n#pragma once\nint f() {\n").has_value());
}

TEST(CodeExtract, BareCodeWithoutFences) {
  auto src = extract_cpp_source("  #pragma once\n#include <vector>\n  ");
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(*src, "#pragma once\n#include <vector>\n");
  EXPECT_FALSE(extract_cpp_source("I cannot help with that.").has_value());
}

TEST(CodeExtract, CrlfLineEndings) {
  auto src = extract_cpp_source("```cpp\r\nint a;\r\n```\r\n");
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(*src, "int a;\n");
}
