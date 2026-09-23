#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gemini {

enum class Role { kUser, kModel };

[[nodiscard]] std::string_view to_string(Role role) noexcept;

struct FunctionCall {
  std::string name;
  nlohmann::json args = nlohmann::json::object();
  std::string id;  // optional correlation id, echoed back in FunctionResponse
};

struct FunctionResponse {
  std::string name;
  nlohmann::json response = nlohmann::json::object();
  std::string id;
};

/// One element of a Content. Only the part kinds this library understands are modelled;
/// unknown kinds (inline data, executable code, ...) are skipped when parsing.
struct Part {
  std::variant<std::string, FunctionCall, FunctionResponse> value;
  bool thought = false;  // true for "thinking" summaries emitted by reasoning models
  // Opaque token that must be sent back verbatim in later turns so the model can
  // resume its reasoning (required for multi-turn function calling on thinking models).
  std::string thought_signature;

  [[nodiscard]] static Part text(std::string text) { return Part{std::move(text)}; }
  [[nodiscard]] static Part call(FunctionCall call) { return Part{std::move(call)}; }
  [[nodiscard]] static Part response(FunctionResponse response) { return Part{std::move(response)}; }

  [[nodiscard]] const std::string* as_text() const { return std::get_if<std::string>(&value); }
  [[nodiscard]] const FunctionCall* as_call() const { return std::get_if<FunctionCall>(&value); }
  [[nodiscard]] const FunctionResponse* as_response() const {
    return std::get_if<FunctionResponse>(&value);
  }
};

struct Content {
  Role role = Role::kUser;
  std::vector<Part> parts;

  [[nodiscard]] static Content user(std::string text);
  [[nodiscard]] static Content model(std::string text);

  /// Concatenation of all non-thought text parts.
  [[nodiscard]] std::string text() const;
};

/// A tool the model may call. `parameters` is an OpenAPI-subset JSON schema.
struct FunctionDeclaration {
  std::string name;
  std::string description;
  nlohmann::json parameters = nlohmann::json::object();
};

struct GenerationConfig {
  std::optional<double> temperature;
  std::optional<double> top_p;
  std::optional<int> top_k;
  std::optional<int> max_output_tokens;
  std::optional<int> candidate_count;
  std::optional<int> seed;
  std::optional<int> thinking_budget;  // 0 disables thinking on models that allow it
  std::optional<std::string> response_mime_type;  // e.g. "application/json"
  nlohmann::json response_schema;                 // null when unused
  std::vector<std::string> stop_sequences;
};

struct SafetySetting {
  std::string category;   // e.g. "HARM_CATEGORY_HARASSMENT"
  std::string threshold;  // e.g. "BLOCK_ONLY_HIGH"
};

struct GenerateRequest {
  std::vector<Content> contents;
  std::optional<Content> system_instruction;
  std::vector<FunctionDeclaration> tools;
  GenerationConfig config;
  std::vector<SafetySetting> safety_settings;
};

struct UsageMetadata {
  std::int64_t prompt_tokens = 0;
  std::int64_t candidates_tokens = 0;
  std::int64_t thoughts_tokens = 0;
  std::int64_t total_tokens = 0;

  UsageMetadata& operator+=(const UsageMetadata& other) noexcept;
};

struct Candidate {
  Content content{Role::kModel, {}};
  std::string finish_reason;  // "STOP", "MAX_TOKENS", "SAFETY", ...
  int index = 0;
};

struct GenerateResponse {
  std::vector<Candidate> candidates;
  UsageMetadata usage;
  std::string model_version;
  std::string response_id;
  std::optional<std::string> prompt_block_reason;

  /// Text of the first candidate (empty if there is none).
  [[nodiscard]] std::string text() const;
  /// Function calls requested by the first candidate.
  [[nodiscard]] std::vector<FunctionCall> function_calls() const;
  [[nodiscard]] std::string finish_reason() const;

  /// Folds a streamed chunk into this (aggregate) response: text parts are appended,
  /// finish reason / usage / model version take the latest non-empty value.
  void merge(const GenerateResponse& chunk);
};

struct ModelInfo {
  std::string name;  // "models/gemini-2.5-flash"
  std::string display_name;
  std::string description;
  std::int64_t input_token_limit = 0;
  std::int64_t output_token_limit = 0;
  std::vector<std::string> supported_methods;
};

}  // namespace gemini
