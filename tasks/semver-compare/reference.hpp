#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace semver_detail {

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool is_ident_char(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

inline bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!is_digit(c)) return false;
  }
  return true;
}

inline bool valid_number(std::string_view s) { return all_digits(s) && (s.size() == 1 || s.front() != '0'); }

inline bool valid_ident(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!is_ident_char(c)) return false;
  }
  return true;
}

inline std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const auto pos = s.find(sep, start);
    parts.push_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return parts;
}

// Both are digit strings without leading zeros: longer means larger.
inline int compare_numbers(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  const int c = a.compare(b);
  return (c > 0) - (c < 0);
}

struct Version {
  std::vector<std::string_view> core;
  std::vector<std::string_view> pre;
};

inline std::optional<Version> parse(std::string_view v) {
  if (const auto plus = v.find('+'); plus != std::string_view::npos) {
    for (auto id : split(v.substr(plus + 1), '.')) {
      if (!valid_ident(id)) return std::nullopt;
    }
    v = v.substr(0, plus);
  }
  Version out;
  if (const auto dash = v.find('-'); dash != std::string_view::npos) {
    for (auto id : split(v.substr(dash + 1), '.')) {
      if (!valid_ident(id)) return std::nullopt;
      if (all_digits(id) && !valid_number(id)) return std::nullopt;
      out.pre.push_back(id);
    }
    v = v.substr(0, dash);
  }
  out.core = split(v, '.');
  if (out.core.size() != 3) return std::nullopt;
  for (auto part : out.core) {
    if (!valid_number(part)) return std::nullopt;
  }
  return out;
}

}  // namespace semver_detail

inline std::optional<int> compare_semver(std::string_view a, std::string_view b) {
  using namespace semver_detail;
  const auto va = parse(a);
  const auto vb = parse(b);
  if (!va || !vb) return std::nullopt;

  for (int i = 0; i < 3; ++i) {
    if (int c = compare_numbers(va->core[i], vb->core[i]); c != 0) return c;
  }
  if (va->pre.empty() || vb->pre.empty()) {
    if (va->pre.empty() && vb->pre.empty()) return 0;
    return va->pre.empty() ? 1 : -1;
  }
  const std::size_t n = std::min(va->pre.size(), vb->pre.size());
  for (std::size_t i = 0; i < n; ++i) {
    const auto x = va->pre[i];
    const auto y = vb->pre[i];
    const bool xn = all_digits(x);
    const bool yn = all_digits(y);
    int c = 0;
    if (xn && yn) c = compare_numbers(x, y);
    else if (xn) c = -1;
    else if (yn) c = 1;
    else c = (x.compare(y) > 0) - (x.compare(y) < 0);
    if (c != 0) return c;
  }
  if (va->pre.size() == vb->pre.size()) return 0;
  return va->pre.size() < vb->pre.size() ? -1 : 1;
}
