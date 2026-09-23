// Minimal tour of the gemini-cpp library API.
//
//   export GEMINI_API_KEY=...
//   ./build/examples/quickstart
#include "gemini/gemini.hpp"

#include <iostream>

int main() {
  auto options = gemini::ClientOptions::from_env();
  if (!options) {
    std::cerr << options.error().describe() << "\n";
    return 1;
  }
  gemini::Client client(std::move(*options));

  // 1. One-shot generation.
  if (auto reply = client.generate("In one sentence: why does RAII matter in C++?")) {
    std::cout << reply->text() << "\n\n";
  } else {
    std::cerr << reply.error().describe() << "\n";
    return 1;
  }

  // 2. Streaming with a system instruction and generation settings.
  gemini::GenerateRequest request;
  request.system_instruction = gemini::Content::user("You are a terse senior C++ reviewer.");
  request.contents.push_back(gemini::Content::user("List three pitfalls of std::shared_ptr."));
  request.config.temperature = 0.3;
  auto streamed = client.stream(request, [](const gemini::GenerateResponse& chunk) {
    std::cout << chunk.text() << std::flush;
    return true;  // return false to cancel
  });
  std::cout << "\n\n";
  if (!streamed) std::cerr << streamed.error().describe() << "\n";

  // 3. Multi-turn chat with automatic function calling.
  gemini::ToolRegistry tools;
  gemini::builtin_tools::register_defaults(tools);
  gemini::ChatSession chat(client, {}, &tools);
  chat.set_tool_observer([](const gemini::FunctionCall& call, const gemini::FunctionResponse& result) {
    std::cout << "[tool] " << call.name << call.args.dump() << " -> " << result.response.dump() << "\n";
  });
  if (auto answer = chat.send("What is (2^32 - 1) / 5? Use a tool.")) {
    std::cout << answer->text() << "\n";
  }

  const auto m = client.metrics();
  std::cout << "\nrequests=" << m.requests << " retries=" << m.retries << " prompt_tokens=" << m.prompt_tokens
            << " output_tokens=" << m.output_tokens << "\n";
  return 0;
}
