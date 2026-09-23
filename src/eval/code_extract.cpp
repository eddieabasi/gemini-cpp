#include "gemini/eval/code_extract.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace gemini::eval {

namespace {

struct Fence {
  char ch = 0;
  std::size_t length = 0;
  std::string info;
};

/// Recognises a fence line: up to three spaces of indentation, then >= 3 backticks/tildes.
std::optional<Fence> parse_fence(std::string_view line) {
  std::size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') ++i;
  if (i >= line.size() || (line[i] != '`' && line[i] != '~')) return std::nullopt;
  Fence fence;
  fence.ch = line[i];
  while (i < line.size() && line[i] == fence.ch) {
    ++fence.length;
    ++i;
  }
  if (fence.length < 3) return std::nullopt;
  std::string_view info = line.substr(i);
  while (!info.empty() && std::isspace(static_cast<unsigned char>(info.front()))) info.remove_prefix(1);
  // Only the first word of the info string is the language (e.g. "cpp title=x").
  info = info.substr(0, std::min(info.find_first_of(" \t{"), info.size()));
  fence.info.reserve(info.size());
  for (char c : info) fence.info.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return fence;
}

bool is_closing(std::string_view line, const Fence& open) {
  std::size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') ++i;
  std::size_t n = 0;
  while (i < line.size() && line[i] == open.ch) {
    ++n;
    ++i;
  }
  if (n < open.length) return false;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  return i == line.size();
}

bool is_cpp_language(std::string_view lang) {
  static constexpr std::array<std::string_view, 8> kNames = {"cpp", "c++", "cc",  "cxx",
                                                              "hpp", "hxx", "h", "c"};
  return std::find(kNames.begin(), kNames.end(), lang) != kNames.end();
}

bool looks_like_code(std::string_view text) {
  return text.find("#include") != std::string_view::npos ||
         text.find("#pragma once") != std::string_view::npos ||
         text.find("template <") != std::string_view::npos ||
         text.find("template<") != std::string_view::npos;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

}  // namespace

std::vector<CodeBlock> find_code_blocks(std::string_view markdown) {
  std::vector<CodeBlock> blocks;
  std::optional<Fence> open;
  CodeBlock current;

  std::size_t pos = 0;
  while (pos <= markdown.size()) {
    auto end = markdown.find('\n', pos);
    if (end == std::string_view::npos) end = markdown.size();
    std::string_view line = markdown.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (!open) {
      if (auto fence = parse_fence(line)) {
        open = std::move(fence);
        current = CodeBlock{open->info, {}, true};
      }
    } else if (is_closing(line, *open)) {
      blocks.push_back(std::move(current));
      open.reset();
    } else {
      current.code.append(line);
      current.code.push_back('\n');
    }
    if (end == markdown.size()) break;
    pos = end + 1;
  }
  if (open) {  // reply truncated mid-block (e.g. MAX_TOKENS): keep what we have
    current.terminated = false;
    blocks.push_back(std::move(current));
  }
  return blocks;
}

std::optional<std::string> extract_cpp_source(std::string_view reply) {
  const auto blocks = find_code_blocks(reply);
  const CodeBlock* best_cpp = nullptr;
  const CodeBlock* best_untagged = nullptr;
  for (const auto& block : blocks) {
    if (trim(block.code).empty()) continue;
    if (is_cpp_language(block.language)) {
      if (best_cpp == nullptr || block.code.size() > best_cpp->code.size()) best_cpp = &block;
    } else if (block.language.empty()) {
      if (best_untagged == nullptr || block.code.size() > best_untagged->code.size()) {
        best_untagged = &block;
      }
    }
  }
  if (best_cpp != nullptr) return best_cpp->code;
  if (best_untagged != nullptr) return best_untagged->code;
  if (blocks.empty() && looks_like_code(reply)) return std::string(trim(reply)) + "\n";
  return std::nullopt;
}

}  // namespace gemini::eval
