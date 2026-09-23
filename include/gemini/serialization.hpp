#pragma once

#include "gemini/error.hpp"
#include "gemini/transport.hpp"
#include "gemini/types.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

/// Mapping between the library's value types and the Gemini REST wire format
/// (https://ai.google.dev/api/generate-content). Kept separate from the client so it can be
/// unit-tested without any networking.
namespace gemini::wire {

[[nodiscard]] nlohmann::json to_json(const Part& part);
[[nodiscard]] nlohmann::json to_json(const Content& content);
[[nodiscard]] nlohmann::json to_json(const GenerateRequest& request);

[[nodiscard]] Result<Part> parse_part(const nlohmann::json& j);
[[nodiscard]] Result<Content> parse_content(const nlohmann::json& j);
[[nodiscard]] Result<GenerateResponse> parse_generate_response(const nlohmann::json& j);
[[nodiscard]] Result<GenerateResponse> parse_generate_response(std::string_view body);
[[nodiscard]] inline Result<GenerateResponse> parse_generate_response(const std::string& body) {
  return parse_generate_response(std::string_view(body));
}
[[nodiscard]] inline Result<GenerateResponse> parse_generate_response(const char* body) {
  return parse_generate_response(std::string_view(body));
}
[[nodiscard]] Result<std::vector<ModelInfo>> parse_model_list(std::string_view body,
                                                              std::string* next_page_token);

/// Builds an Error from a non-2xx HTTP response, extracting the Google RPC status and any
/// RetryInfo / Retry-After hints.
[[nodiscard]] Error parse_api_error(int http_status, std::string_view body,
                                    const HttpHeaders& headers = {});

/// Parses protobuf Duration strings such as "30s" or "1.5s".
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_proto_duration(std::string_view text);

}  // namespace gemini::wire
