#include "gemini/types.hpp"

#include <algorithm>

namespace gemini {

std::string_view to_string(Role role) noexcept {
  return role == Role::kModel ? "model" : "user";
}

Content Content::user(std::string text) { return Content{Role::kUser, {Part::text(std::move(text))}}; }

Content Content::model(std::string text) {
  return Content{Role::kModel, {Part::text(std::move(text))}};
}

std::string Content::text() const {
  std::string out;
  for (const auto& part : parts) {
    if (const auto* t = part.as_text(); t != nullptr && !part.thought) out += *t;
  }
  return out;
}

UsageMetadata& UsageMetadata::operator+=(const UsageMetadata& other) noexcept {
  prompt_tokens += other.prompt_tokens;
  candidates_tokens += other.candidates_tokens;
  thoughts_tokens += other.thoughts_tokens;
  total_tokens += other.total_tokens;
  return *this;
}

std::string GenerateResponse::text() const {
  return candidates.empty() ? std::string{} : candidates.front().content.text();
}

std::vector<FunctionCall> GenerateResponse::function_calls() const {
  std::vector<FunctionCall> calls;
  if (candidates.empty()) return calls;
  for (const auto& part : candidates.front().content.parts) {
    if (const auto* call = part.as_call()) calls.push_back(*call);
  }
  return calls;
}

std::string GenerateResponse::finish_reason() const {
  return candidates.empty() ? std::string{} : candidates.front().finish_reason;
}

namespace {

// Streamed text arrives in fragments; glue fragments of the same kind back together so the
// aggregate looks like a unary response (and history stays compact).
void append_part(std::vector<Part>& parts, const Part& incoming) {
  const auto* incoming_text = incoming.as_text();
  if (incoming_text != nullptr && !parts.empty()) {
    Part& last = parts.back();
    if (auto* last_text = std::get_if<std::string>(&last.value);
        last_text != nullptr && last.thought == incoming.thought &&
        (last.thought_signature.empty() || incoming.thought_signature.empty())) {
      *last_text += *incoming_text;
      if (last.thought_signature.empty()) last.thought_signature = incoming.thought_signature;
      return;
    }
  }
  parts.push_back(incoming);
}

}  // namespace

void GenerateResponse::merge(const GenerateResponse& chunk) {
  for (const auto& incoming : chunk.candidates) {
    auto it = std::find_if(candidates.begin(), candidates.end(),
                           [&](const Candidate& c) { return c.index == incoming.index; });
    if (it == candidates.end()) {
      candidates.push_back(Candidate{Content{Role::kModel, {}}, {}, incoming.index});
      it = std::prev(candidates.end());
    }
    for (const auto& part : incoming.content.parts) append_part(it->content.parts, part);
    if (!incoming.finish_reason.empty()) it->finish_reason = incoming.finish_reason;
  }
  // Usage metadata in a stream is cumulative, so the latest non-empty value wins.
  if (chunk.usage.total_tokens != 0 || chunk.usage.prompt_tokens != 0) usage = chunk.usage;
  if (!chunk.model_version.empty()) model_version = chunk.model_version;
  if (!chunk.response_id.empty()) response_id = chunk.response_id;
  if (chunk.prompt_block_reason) prompt_block_reason = chunk.prompt_block_reason;
}

}  // namespace gemini
