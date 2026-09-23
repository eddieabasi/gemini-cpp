#pragma once

#include "gemini/error.hpp"
#include "gemini/types.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace gemini {

using ToolHandler = std::function<Result<nlohmann::json>(const nlohmann::json& args)>;

/// Maps function declarations advertised to the model onto local C++ handlers.
class ToolRegistry {
 public:
  void add(FunctionDeclaration declaration, ToolHandler handler);

  [[nodiscard]] bool empty() const noexcept { return tools_.empty(); }
  [[nodiscard]] std::vector<FunctionDeclaration> declarations() const;

  /// Runs the handler for `call`. Never throws: unknown tools, handler errors and handler
  /// exceptions are reported back to the model as {"error": "..."} so it can recover.
  [[nodiscard]] FunctionResponse invoke(const FunctionCall& call) const;

 private:
  std::vector<std::pair<FunctionDeclaration, ToolHandler>> tools_;
};

namespace builtin_tools {

/// Registers `get_current_time` and `evaluate_expression`.
void register_defaults(ToolRegistry& registry);

/// Safe arithmetic evaluator (no eval/exec): + - * / % ^, parentheses, unary minus,
/// constants pi/e and functions sqrt, abs, sin, cos, tan, log, log10, exp, floor, ceil.
[[nodiscard]] Result<double> evaluate_expression(std::string_view expression);

}  // namespace builtin_tools

}  // namespace gemini
