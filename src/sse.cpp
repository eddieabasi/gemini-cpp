#include "gemini/sse.hpp"

#include <utility>

namespace gemini {

SseParser::SseParser(EventHandler handler) : handler_(std::move(handler)) {}

bool SseParser::feed(std::string_view bytes) {
  buffer_.append(bytes);
  std::size_t start = 0;
  bool keep_going = true;
  while (keep_going) {
    const auto newline = buffer_.find('\n', start);
    if (newline == std::string::npos) break;
    std::string_view line(buffer_.data() + start, newline - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    keep_going = process_line(line);
    start = newline + 1;
  }
  buffer_.erase(0, start);
  return keep_going;
}

bool SseParser::finish() {
  if (!buffer_.empty()) {
    std::string rest = std::move(buffer_);
    buffer_.clear();
    if (!rest.empty() && rest.back() == '\r') rest.pop_back();
    if (!process_line(rest)) return false;
  }
  return dispatch();
}

bool SseParser::process_line(std::string_view line) {
  if (line.empty()) return dispatch();
  if (line.front() == ':') return true;  // comment / keep-alive

  std::string_view field = line;
  std::string_view value;
  if (const auto colon = line.find(':'); colon != std::string_view::npos) {
    field = line.substr(0, colon);
    value = line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
  }
  if (field == "data") {
    if (has_data_) data_ += '\n';
    data_.append(value);
    has_data_ = true;
  }
  // "event", "id" and "retry" are not used by the Gemini API.
  return true;
}

bool SseParser::dispatch() {
  if (!has_data_) return true;
  std::string data = std::move(data_);
  data_.clear();
  has_data_ = false;
  return handler_(data);
}

}  // namespace gemini
