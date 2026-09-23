#include "gemini/chat.hpp"

#include "gemini/logging.hpp"

namespace gemini {

ChatSession::ChatSession(Client& client, ChatOptions options, const ToolRegistry* tools)
    : client_(client), options_(std::move(options)), tools_(tools) {}

void ChatSession::reset() noexcept {
  history_.clear();
  usage_ = {};
}

GenerateRequest ChatSession::build_request() const {
  GenerateRequest request;
  request.contents = history_;
  if (options_.system_instruction) {
    request.system_instruction = Content::user(*options_.system_instruction);
  }
  if (tools_ != nullptr) request.tools = tools_->declarations();
  request.config = options_.config;
  return request;
}

Result<GenerateResponse> ChatSession::send(std::string message,
                                           const Client::StreamCallback& on_chunk) {
  const std::size_t checkpoint = history_.size();
  auto rollback = [&](Error err) -> Result<GenerateResponse> {
    history_.resize(checkpoint);
    return std::unexpected(std::move(err));
  };

  history_.push_back(Content::user(std::move(message)));

  for (int round = 0; round <= options_.max_tool_rounds; ++round) {
    const GenerateRequest request = build_request();
    auto response = on_chunk ? client_.stream(request, on_chunk) : client_.generate(request);
    if (!response) return rollback(response.error());
    usage_ += response->usage;

    if (response->candidates.empty()) {
      return rollback(Error{ErrorCode::kParse, "response has no candidates", 0, {}, std::nullopt});
    }
    Content reply = response->candidates.front().content;
    reply.role = Role::kModel;
    history_.push_back(std::move(reply));

    const auto calls = response->function_calls();
    if (calls.empty() || tools_ == nullptr) return response;

    Content tool_results{Role::kUser, {}};
    for (const auto& call : calls) {
      log::debug("chat.tool_call", {{"name", call.name}, {"args", call.args.dump()}});
      FunctionResponse result = tools_->invoke(call);
      if (tool_observer_) tool_observer_(call, result);
      tool_results.parts.push_back(Part::response(std::move(result)));
    }
    history_.push_back(std::move(tool_results));
  }

  return rollback(Error{ErrorCode::kInvalidArgument,
                        "model exceeded max_tool_rounds (" +
                            std::to_string(options_.max_tool_rounds) + ") without answering",
                        0, {}, std::nullopt});
}

}  // namespace gemini
