#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

inline std::optional<std::chrono::nanoseconds> parse_duration(std::string_view s) {
  using u64 = std::uint64_t;
  using u128 = unsigned __int128;
  constexpr u64 kLimit = u64{1} << 63;  // |INT64_MIN|
  auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

  bool negative = false;
  if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s == "0") return std::chrono::nanoseconds{0};
  if (s.empty()) return std::nullopt;

  u128 total = 0;
  while (!s.empty()) {
    std::size_t i = 0;
    bool any_digit = false;
    u128 whole = 0;
    for (; i < s.size() && is_digit(s[i]); ++i) {
      any_digit = true;
      whole = whole * 10 + static_cast<u64>(s[i] - '0');
      if (whole > kLimit) return std::nullopt;
    }
    u64 frac = 0;
    u64 scale = 1;
    if (i < s.size() && s[i] == '.') {
      for (++i; i < s.size() && is_digit(s[i]); ++i) {
        any_digit = true;
        if (scale < 1'000'000'000'000'000'000ULL) {  // further digits are below 1ns
          frac = frac * 10 + static_cast<u64>(s[i] - '0');
          scale *= 10;
        }
      }
    }
    if (!any_digit) return std::nullopt;
    s.remove_prefix(i);

    std::size_t u = 0;
    while (u < s.size() && s[u] != '.' && !is_digit(s[u])) ++u;
    const std::string_view unit_name = s.substr(0, u);
    u64 unit = 0;
    if (unit_name == "ns") unit = 1;
    else if (unit_name == "us" || unit_name == "\xC2\xB5s" || unit_name == "\xCE\xBCs") unit = 1'000;
    else if (unit_name == "ms") unit = 1'000'000;
    else if (unit_name == "s") unit = 1'000'000'000;
    else if (unit_name == "m") unit = 60'000'000'000ULL;
    else if (unit_name == "h") unit = 3'600'000'000'000ULL;
    else return std::nullopt;
    s.remove_prefix(u);

    total += whole * unit + (u128{frac} * unit) / scale;
    if (total > kLimit) return std::nullopt;
  }
  if (!negative && total == kLimit) return std::nullopt;
  if (negative) {
    if (total == kLimit) return std::chrono::nanoseconds{INT64_MIN};
    return std::chrono::nanoseconds{-static_cast<std::int64_t>(total)};
  }
  return std::chrono::nanoseconds{static_cast<std::int64_t>(total)};
}
