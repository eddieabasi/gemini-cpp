#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cli {

/// Small, dependency-free argument parser: `--name value`, `--name=value`, boolean
/// `--flag`, repeated options, and `--` to end option parsing.
class Args {
 public:
  struct Spec {
    std::set<std::string> value_options;  // options that take a value
    std::set<std::string> flags;          // boolean options
  };

  /// Parses argv[first..argc). Returns an error message on failure.
  [[nodiscard]] std::optional<std::string> parse(int argc, char** argv, int first, const Spec& spec);

  [[nodiscard]] bool flag(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> get(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> get_all(std::string_view name) const;
  [[nodiscard]] std::string get_or(std::string_view name, std::string fallback) const;
  [[nodiscard]] const std::vector<std::string>& positional() const noexcept { return positional_; }

  /// Parses an option as a number; returns an error message on malformed input.
  [[nodiscard]] std::optional<std::string> get_int(std::string_view name, std::optional<int>& out) const;
  [[nodiscard]] std::optional<std::string> get_double(std::string_view name,
                                                      std::optional<double>& out) const;

 private:
  std::multimap<std::string, std::string, std::less<>> values_;
  std::set<std::string, std::less<>> flags_;
  std::vector<std::string> positional_;
};

}  // namespace cli
