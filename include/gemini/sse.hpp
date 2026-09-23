#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace gemini {

/// Incremental parser for the text/event-stream format used by `?alt=sse`.
/// Bytes can arrive split at arbitrary boundaries; complete events are dispatched to the
/// handler as soon as their terminating blank line is seen.
class SseParser {
 public:
  /// Receives the (possibly multi-line) `data:` payload of each event. Return false to stop.
  using EventHandler = std::function<bool(std::string_view data)>;

  explicit SseParser(EventHandler handler);

  /// Feeds more bytes. Returns false if the handler asked to stop.
  bool feed(std::string_view bytes);
  /// Flushes a trailing event that was not terminated by a blank line.
  bool finish();

 private:
  bool process_line(std::string_view line);
  bool dispatch();

  EventHandler handler_;
  std::string buffer_;
  std::string data_;
  bool has_data_ = false;
};

}  // namespace gemini
