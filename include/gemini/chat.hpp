#pragma once

#include "gemini/client.hpp"
#include "gemini/tools.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gemini {

struct ChatOptions {
  std::optional<std::string> system_instruction;
  GenerationConfig config;
  /// Upper bound on model -> tool -> model round trips within a single send().
  int max_tool_rounds = 8;
};

/// Multi-turn conversation with optional automatic function calling.
/// Not thread-safe; use one session per conversation.
class ChatSession {
 public:
  using ToolObserver = std::function<void(const FunctionCall&, const FunctionResponse&)>;

  explicit ChatSession(Client& client, ChatOptions options = {},
                       const ToolRegistry* tools = nullptr);

  /// Sends a user message and returns the model's final reply. If the model requests
  /// function calls, they are executed and fed back until it produces a normal answer.
  /// On error the history is rolled back so the session stays consistent.
  /// If `on_chunk` is set the reply is streamed.
  [[nodiscard]] Result<GenerateResponse> send(std::string message,
                                              const Client::StreamCallback& on_chunk = nullptr);

  void reset() noexcept;
  void set_tool_observer(ToolObserver observer) { tool_observer_ = std::move(observer); }

  [[nodiscard]] const std::vector<Content>& history() const noexcept { return history_; }
  [[nodiscard]] const UsageMetadata& usage() const noexcept { return usage_; }

 private:
  [[nodiscard]] GenerateRequest build_request() const;

  Client& client_;
  ChatOptions options_;
  const ToolRegistry* tools_;
  ToolObserver tool_observer_;
  std::vector<Content> history_;
  UsageMetadata usage_;
};

}  // namespace gemini
