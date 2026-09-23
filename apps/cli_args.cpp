#include "cli_args.hpp"

#include <charconv>
#include <cstdlib>

namespace cli {

std::optional<std::string> Args::parse(int argc, char** argv, int first, const Spec& spec) {
  bool options_done = false;
  for (int i = first; i < argc; ++i) {
    std::string arg = argv[i];
    if (options_done || !arg.starts_with("--") || arg == "-") {
      positional_.push_back(std::move(arg));
      continue;
    }
    if (arg == "--") {
      options_done = true;
      continue;
    }
    std::string name = arg.substr(2);
    std::optional<std::string> inline_value;
    if (const auto eq = name.find('='); eq != std::string::npos) {
      inline_value = name.substr(eq + 1);
      name.resize(eq);
    }
    if (spec.flags.contains(name)) {
      if (inline_value) return "option --" + name + " does not take a value";
      flags_.insert(name);
    } else if (spec.value_options.contains(name)) {
      if (inline_value) {
        values_.emplace(name, *inline_value);
      } else if (i + 1 < argc) {
        values_.emplace(name, argv[++i]);
      } else {
        return "option --" + name + " requires a value";
      }
    } else {
      return "unknown option --" + name;
    }
  }
  return std::nullopt;
}

bool Args::flag(std::string_view name) const { return flags_.find(name) != flags_.end(); }

std::optional<std::string> Args::get(std::string_view name) const {
  // Last occurrence wins for single-valued options.
  auto [begin, end] = values_.equal_range(name);
  if (begin == end) return std::nullopt;
  return std::prev(end)->second;
}

std::vector<std::string> Args::get_all(std::string_view name) const {
  std::vector<std::string> out;
  auto [begin, end] = values_.equal_range(name);
  for (auto it = begin; it != end; ++it) out.push_back(it->second);
  return out;
}

std::string Args::get_or(std::string_view name, std::string fallback) const {
  auto v = get(name);
  return v ? *v : std::move(fallback);
}

std::optional<std::string> Args::get_int(std::string_view name, std::optional<int>& out) const {
  auto v = get(name);
  if (!v) return std::nullopt;
  int value = 0;
  auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), value);
  if (ec != std::errc{} || ptr != v->data() + v->size()) {
    return "--" + std::string(name) + " expects an integer, got '" + *v + "'";
  }
  out = value;
  return std::nullopt;
}

std::optional<std::string> Args::get_double(std::string_view name, std::optional<double>& out) const {
  auto v = get(name);
  if (!v) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(v->c_str(), &end);
  if (v->empty() || end != v->c_str() + v->size()) {
    return "--" + std::string(name) + " expects a number, got '" + *v + "'";
  }
  out = value;
  return std::nullopt;
}

}  // namespace cli
