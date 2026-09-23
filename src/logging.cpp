#include "gemini/logging.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace gemini::log {

namespace {

std::atomic<Level> g_level{Level::kWarn};
std::atomic<bool> g_json{false};
std::mutex g_mutex;

std::string_view level_name(Level level) {
  switch (level) {
    case Level::kDebug: return "debug";
    case Level::kInfo: return "info";
    case Level::kWarn: return "warn";
    case Level::kError: return "error";
    case Level::kOff: return "off";
  }
  return "?";
}

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto secs = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char buf[40];
  const auto n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + n, sizeof buf - n, ".%03dZ", static_cast<int>(ms.count()));
  return buf;
}

bool needs_quotes(const std::string& v) {
  return v.empty() || v.find_first_of(" \t\"=") != std::string::npos;
}

}  // namespace

void set_level(Level level) noexcept { g_level.store(level); }
Level level() noexcept { return g_level.load(); }
void set_json(bool enabled) noexcept { g_json.store(enabled); }
bool enabled(Level l) noexcept { return l != Level::kOff && l >= g_level.load(); }

bool parse_level(std::string_view text, Level& out) noexcept {
  for (auto l : {Level::kDebug, Level::kInfo, Level::kWarn, Level::kError, Level::kOff}) {
    if (text == level_name(l)) {
      out = l;
      return true;
    }
  }
  return false;
}

void write(Level lvl, std::string_view event, std::initializer_list<Field> fields) {
  std::string line;
  if (g_json.load()) {
    nlohmann::json j;
    j["ts"] = timestamp();
    j["level"] = level_name(lvl);
    j["event"] = event;
    for (const auto& [k, v] : fields) j[std::string(k)] = v;
    line = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  } else {
    line = timestamp();
    line += ' ';
    std::string name(level_name(lvl));
    for (auto& c : name) c = static_cast<char>(c - ('a' - 'A'));
    line += name;
    line += ' ';
    line += event;
    for (const auto& [k, v] : fields) {
      line += ' ';
      line += k;
      line += '=';
      line += needs_quotes(v) ? nlohmann::json(v).dump(-1, ' ', false,
                                                       nlohmann::json::error_handler_t::replace)
                              : v;
    }
  }
  line += '\n';
  std::lock_guard lock(g_mutex);
  std::fputs(line.c_str(), stderr);
}

}  // namespace gemini::log
