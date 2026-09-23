#include "gemini/serialization.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace gemini::wire {

using nlohmann::json;

namespace {

std::string string_or(const json& j, const char* key, std::string fallback = {}) {
  if (auto it = j.find(key); it != j.end() && it->is_string()) return it->get<std::string>();
  return fallback;
}

std::int64_t int_or(const json& j, const char* key, std::int64_t fallback = 0) {
  if (auto it = j.find(key); it != j.end() && it->is_number_integer()) {
    return it->get<std::int64_t>();
  }
  return fallback;
}

ErrorCode code_for_status(int status, std::string_view api_status) {
  if (api_status == "RESOURCE_EXHAUSTED") return ErrorCode::kRateLimited;
  if (api_status == "DEADLINE_EXCEEDED") return ErrorCode::kTimeout;
  switch (status) {
    case 400: return ErrorCode::kInvalidArgument;
    case 401:
    case 403: return ErrorCode::kUnauthenticated;
    case 404: return ErrorCode::kNotFound;
    case 408: return ErrorCode::kTimeout;
    case 429: return ErrorCode::kRateLimited;
    default: break;
  }
  if (status >= 500 && status <= 599) return ErrorCode::kServer;
  return ErrorCode::kHttp;
}

json config_to_json(const GenerationConfig& c) {
  json j = json::object();
  if (c.temperature) j["temperature"] = *c.temperature;
  if (c.top_p) j["topP"] = *c.top_p;
  if (c.top_k) j["topK"] = *c.top_k;
  if (c.max_output_tokens) j["maxOutputTokens"] = *c.max_output_tokens;
  if (c.candidate_count) j["candidateCount"] = *c.candidate_count;
  if (c.seed) j["seed"] = *c.seed;
  if (c.response_mime_type) j["responseMimeType"] = *c.response_mime_type;
  if (!c.response_schema.is_null()) j["responseSchema"] = c.response_schema;
  if (!c.stop_sequences.empty()) j["stopSequences"] = c.stop_sequences;
  if (c.thinking_budget) j["thinkingConfig"] = {{"thinkingBudget", *c.thinking_budget}};
  return j;
}

}  // namespace

json to_json(const Part& part) {
  json j;
  if (const auto* text = part.as_text()) {
    j["text"] = *text;
    if (part.thought) j["thought"] = true;
  } else if (const auto* call = part.as_call()) {
    j["functionCall"] = {{"name", call->name}, {"args", call->args}};
    if (!call->id.empty()) j["functionCall"]["id"] = call->id;
  } else if (const auto* resp = part.as_response()) {
    j["functionResponse"] = {{"name", resp->name}, {"response", resp->response}};
    if (!resp->id.empty()) j["functionResponse"]["id"] = resp->id;
  }
  if (!part.thought_signature.empty()) j["thoughtSignature"] = part.thought_signature;
  return j;
}

json to_json(const Content& content) {
  json parts = json::array();
  for (const auto& p : content.parts) parts.push_back(to_json(p));
  return {{"role", to_string(content.role)}, {"parts", std::move(parts)}};
}

json to_json(const GenerateRequest& request) {
  json j;
  j["contents"] = json::array();
  for (const auto& c : request.contents) j["contents"].push_back(to_json(c));

  if (request.system_instruction) {
    json parts = json::array();
    for (const auto& p : request.system_instruction->parts) parts.push_back(to_json(p));
    j["systemInstruction"] = {{"parts", std::move(parts)}};
  }

  if (!request.tools.empty()) {
    json decls = json::array();
    for (const auto& d : request.tools) {
      json decl = {{"name", d.name}, {"description", d.description}};
      if (!d.parameters.is_null() && !d.parameters.empty()) decl["parameters"] = d.parameters;
      decls.push_back(std::move(decl));
    }
    j["tools"] = json::array({{{"functionDeclarations", std::move(decls)}}});
  }

  if (json cfg = config_to_json(request.config); !cfg.empty()) j["generationConfig"] = std::move(cfg);

  if (!request.safety_settings.empty()) {
    json safety = json::array();
    for (const auto& s : request.safety_settings) {
      safety.push_back({{"category", s.category}, {"threshold", s.threshold}});
    }
    j["safetySettings"] = std::move(safety);
  }
  return j;
}

Result<Part> parse_part(const json& j) {
  if (!j.is_object()) return make_error(ErrorCode::kParse, "part is not an object");
  Part part;
  part.thought_signature = string_or(j, "thoughtSignature");
  if (auto it = j.find("text"); it != j.end() && it->is_string()) {
    part.value = it->get<std::string>();
    part.thought = j.value("thought", false);
    return part;
  }
  if (auto it = j.find("functionCall"); it != j.end() && it->is_object()) {
    FunctionCall call;
    call.name = string_or(*it, "name");
    call.id = string_or(*it, "id");
    if (auto args = it->find("args"); args != it->end()) call.args = *args;
    if (call.name.empty()) return make_error(ErrorCode::kParse, "functionCall without name");
    part.value = std::move(call);
    return part;
  }
  if (auto it = j.find("functionResponse"); it != j.end() && it->is_object()) {
    FunctionResponse resp;
    resp.name = string_or(*it, "name");
    resp.id = string_or(*it, "id");
    if (auto r = it->find("response"); r != it->end()) resp.response = *r;
    part.value = std::move(resp);
    return part;
  }
  return make_error(ErrorCode::kParse, "unsupported part kind");
}

Result<Content> parse_content(const json& j) {
  Content content{Role::kModel, {}};
  if (!j.is_object()) return content;
  content.role = string_or(j, "role", "model") == "user" ? Role::kUser : Role::kModel;
  if (auto parts = j.find("parts"); parts != j.end() && parts->is_array()) {
    for (const auto& pj : *parts) {
      auto part = parse_part(pj);
      // Unknown kinds (inlineData, executableCode, ...) are skipped rather than failing
      // the whole response; malformed known kinds are real errors.
      if (part) {
        content.parts.push_back(std::move(*part));
      } else if (part.error().message != "unsupported part kind") {
        return std::unexpected(part.error());
      }
    }
  }
  return content;
}

Result<GenerateResponse> parse_generate_response(const json& j) {
  if (!j.is_object()) return make_error(ErrorCode::kParse, "response is not a JSON object");
  GenerateResponse out;
  if (auto cands = j.find("candidates"); cands != j.end() && cands->is_array()) {
    int position = 0;
    for (const auto& cj : *cands) {
      Candidate cand;
      cand.index = static_cast<int>(int_or(cj, "index", position++));
      cand.finish_reason = string_or(cj, "finishReason");
      if (auto content = cj.find("content"); content != cj.end()) {
        auto parsed = parse_content(*content);
        if (!parsed) return std::unexpected(parsed.error());
        cand.content = std::move(*parsed);
      }
      out.candidates.push_back(std::move(cand));
    }
  }
  if (auto usage = j.find("usageMetadata"); usage != j.end() && usage->is_object()) {
    out.usage.prompt_tokens = int_or(*usage, "promptTokenCount");
    out.usage.candidates_tokens = int_or(*usage, "candidatesTokenCount");
    out.usage.thoughts_tokens = int_or(*usage, "thoughtsTokenCount");
    out.usage.total_tokens = int_or(*usage, "totalTokenCount");
  }
  out.model_version = string_or(j, "modelVersion");
  out.response_id = string_or(j, "responseId");
  if (auto fb = j.find("promptFeedback"); fb != j.end() && fb->is_object()) {
    if (auto reason = string_or(*fb, "blockReason"); !reason.empty()) {
      out.prompt_block_reason = std::move(reason);
    }
  }
  return out;
}

Result<GenerateResponse> parse_generate_response(std::string_view body) {
  json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    return make_error(ErrorCode::kParse,
                      "invalid JSON in response: " + std::string(body.substr(0, 200)));
  }
  return parse_generate_response(j);
}

Result<std::vector<ModelInfo>> parse_model_list(std::string_view body,
                                                std::string* next_page_token) {
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return make_error(ErrorCode::kParse, "invalid JSON in model list");
  }
  std::vector<ModelInfo> models;
  if (auto arr = j.find("models"); arr != j.end() && arr->is_array()) {
    for (const auto& m : *arr) {
      ModelInfo info;
      info.name = string_or(m, "name");
      info.display_name = string_or(m, "displayName");
      info.description = string_or(m, "description");
      info.input_token_limit = int_or(m, "inputTokenLimit");
      info.output_token_limit = int_or(m, "outputTokenLimit");
      if (auto methods = m.find("supportedGenerationMethods");
          methods != m.end() && methods->is_array()) {
        for (const auto& method : *methods) {
          if (method.is_string()) info.supported_methods.push_back(method.get<std::string>());
        }
      }
      models.push_back(std::move(info));
    }
  }
  if (next_page_token != nullptr) *next_page_token = string_or(j, "nextPageToken");
  return models;
}

std::optional<std::chrono::milliseconds> parse_proto_duration(std::string_view text) {
  if (text.size() < 2 || text.back() != 's') return std::nullopt;
  const std::string number(text.substr(0, text.size() - 1));
  char* end = nullptr;
  const double seconds = std::strtod(number.c_str(), &end);
  if (end != number.c_str() + number.size() || !std::isfinite(seconds) || seconds < 0) {
    return std::nullopt;
  }
  return std::chrono::milliseconds(static_cast<std::int64_t>(std::ceil(seconds * 1000.0)));
}

Error parse_api_error(int http_status, std::string_view body, const HttpHeaders& headers) {
  Error err;
  err.http_status = http_status;
  err.message = "HTTP " + std::to_string(http_status);

  json j = json::parse(body, nullptr, false);
  if (!j.is_discarded() && j.is_array() && !j.empty()) j = j.front();  // streaming errors
  if (!j.is_discarded() && j.is_object()) {
    if (auto e = j.find("error"); e != j.end() && e->is_object()) {
      if (auto msg = string_or(*e, "message"); !msg.empty()) err.message = std::move(msg);
      err.api_status = string_or(*e, "status");
      if (auto details = e->find("details"); details != e->end() && details->is_array()) {
        for (const auto& d : *details) {
          if (string_or(d, "@type").ends_with("google.rpc.RetryInfo")) {
            err.retry_after = parse_proto_duration(string_or(d, "retryDelay"));
          }
        }
      }
    }
  } else if (!body.empty()) {
    err.message += ": " + std::string(body.substr(0, 300));
  }

  if (!err.retry_after) {
    if (auto it = headers.find("retry-after"); it != headers.end()) {
      std::int64_t seconds = 0;
      const auto& v = it->second;
      if (auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), seconds);
          ec == std::errc{} && seconds >= 0) {
        err.retry_after = std::chrono::seconds(seconds);
      }
    }
  }
  err.code = code_for_status(http_status, err.api_status);
  return err;
}

}  // namespace gemini::wire
