#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gemini::eval {

struct CodeBlock {
  std::string language;  // info string after the fence, lower-cased ("cpp", "c++", "")
  std::string code;
  bool terminated = true;  // false if the reply was cut off inside the block
};

/// Finds fenced (``` or ~~~) code blocks in a Markdown reply.
[[nodiscard]] std::vector<CodeBlock> find_code_blocks(std::string_view markdown);

/// Picks the most plausible C++ source out of a model reply: the longest C++-tagged block,
/// else the longest untagged block, else the whole reply if it looks like bare code.
[[nodiscard]] std::optional<std::string> extract_cpp_source(std::string_view reply);

}  // namespace gemini::eval
