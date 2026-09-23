#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

/// Minimal structured logger writing to stderr, either as `key=value` text or JSON lines
/// (for shipping to a log pipeline). Thread-safe.
namespace gemini::log {

enum class Level { kDebug, kInfo, kWarn, kError, kOff };

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
void set_json(bool enabled) noexcept;
[[nodiscard]] bool parse_level(std::string_view text, Level& out) noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;

using Field = std::pair<std::string_view, std::string>;

void write(Level level, std::string_view event, std::initializer_list<Field> fields = {});

inline void debug(std::string_view event, std::initializer_list<Field> fields = {}) {
  if (enabled(Level::kDebug)) write(Level::kDebug, event, fields);
}
inline void info(std::string_view event, std::initializer_list<Field> fields = {}) {
  if (enabled(Level::kInfo)) write(Level::kInfo, event, fields);
}
inline void warn(std::string_view event, std::initializer_list<Field> fields = {}) {
  if (enabled(Level::kWarn)) write(Level::kWarn, event, fields);
}
inline void error(std::string_view event, std::initializer_list<Field> fields = {}) {
  if (enabled(Level::kError)) write(Level::kError, event, fields);
}

}  // namespace gemini::log
