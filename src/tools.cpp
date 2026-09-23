#include "gemini/tools.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <numbers>
#include <string>

namespace gemini {

using nlohmann::json;

void ToolRegistry::add(FunctionDeclaration declaration, ToolHandler handler) {
  tools_.emplace_back(std::move(declaration), std::move(handler));
}

std::vector<FunctionDeclaration> ToolRegistry::declarations() const {
  std::vector<FunctionDeclaration> out;
  out.reserve(tools_.size());
  for (const auto& [decl, handler] : tools_) out.push_back(decl);
  return out;
}

FunctionResponse ToolRegistry::invoke(const FunctionCall& call) const {
  FunctionResponse response{call.name, json::object(), call.id};
  for (const auto& [decl, handler] : tools_) {
    if (decl.name != call.name) continue;
    try {
      auto result = handler(call.args);
      if (!result) {
        response.response = {{"error", result.error().message}};
      } else if (result->is_object()) {
        response.response = std::move(*result);
      } else {
        response.response = {{"result", std::move(*result)}};
      }
    } catch (const std::exception& e) {
      response.response = {{"error", std::string("tool threw: ") + e.what()}};
    }
    return response;
  }
  response.response = {{"error", "unknown function: " + call.name}};
  return response;
}

namespace builtin_tools {

namespace {

/// Recursive-descent parser:
///   expr    := term (('+' | '-') term)*
///   term    := unary (('*' | '/' | '%') unary)*
///   unary   := ('+' | '-') unary | power
///   power   := primary ('^' unary)?          (right associative)
///   primary := number | ident | ident '(' expr ')' | '(' expr ')'
class ExpressionParser {
 public:
  explicit ExpressionParser(std::string_view text) : text_(text) {}

  Result<double> parse() {
    auto value = expr();
    if (!value) return value;
    skip_space();
    if (pos_ != text_.size()) return fail("unexpected character '" + std::string(1, text_[pos_]) + "'");
    if (!std::isfinite(*value)) return fail("result is not a finite number");
    return value;
  }

 private:
  static constexpr int kMaxDepth = 200;

  std::unexpected<Error> fail(std::string message) const {
    return make_error(ErrorCode::kInvalidArgument,
                      message + " at position " + std::to_string(pos_));
  }

  void skip_space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) ++pos_;
  }

  bool accept(char c) {
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  Result<double> expr() {
    if (++depth_ > kMaxDepth) return fail("expression nested too deeply");
    auto lhs = term();
    while (lhs) {
      if (accept('+')) {
        auto rhs = term();
        if (!rhs) return rhs;
        *lhs += *rhs;
      } else if (accept('-')) {
        auto rhs = term();
        if (!rhs) return rhs;
        *lhs -= *rhs;
      } else {
        break;
      }
    }
    --depth_;
    return lhs;
  }

  Result<double> term() {
    auto lhs = unary();
    while (lhs) {
      char op = 0;
      if (accept('*')) op = '*';
      else if (accept('/')) op = '/';
      else if (accept('%')) op = '%';
      else break;
      auto rhs = unary();
      if (!rhs) return rhs;
      if ((op == '/' || op == '%') && *rhs == 0.0) return fail("division by zero");
      if (op == '*') *lhs *= *rhs;
      else if (op == '/') *lhs /= *rhs;
      else *lhs = std::fmod(*lhs, *rhs);
    }
    return lhs;
  }

  Result<double> unary() {
    if (++depth_ > kMaxDepth) return fail("expression nested too deeply");
    Result<double> out;
    if (accept('-')) {
      out = unary();
      if (out) *out = -*out;
    } else if (accept('+')) {
      out = unary();
    } else {
      out = power();
    }
    --depth_;
    return out;
  }

  Result<double> power() {
    auto base = primary();
    if (!base) return base;
    if (accept('^')) {
      auto exponent = unary();
      if (!exponent) return exponent;
      return std::pow(*base, *exponent);
    }
    return base;
  }

  Result<double> primary() {
    skip_space();
    if (pos_ >= text_.size()) return fail("unexpected end of expression");
    if (accept('(')) {
      auto inner = expr();
      if (!inner) return inner;
      if (!accept(')')) return fail("expected ')'");
      return inner;
    }
    const char c = text_[pos_];
    if ((c >= '0' && c <= '9') || c == '.') return number();
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return identifier();
    return fail("unexpected character '" + std::string(1, c) + "'");
  }

  Result<double> number() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() &&
           ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.')) {
      ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      std::size_t look = pos_ + 1;
      if (look < text_.size() && (text_[look] == '+' || text_[look] == '-')) ++look;
      if (look < text_.size() && text_[look] >= '0' && text_[look] <= '9') {
        pos_ = look;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      }
    }
    const std::string token(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size()) {
      pos_ = start;
      return fail("malformed number '" + token + "'");
    }
    return value;
  }

  Result<double> identifier() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() &&
           ((text_[pos_] >= 'a' && text_[pos_] <= 'z') || (text_[pos_] >= 'A' && text_[pos_] <= 'Z') ||
            (text_[pos_] >= '0' && text_[pos_] <= '9'))) {
      ++pos_;
    }
    const std::string_view name = text_.substr(start, pos_ - start);
    if (name == "pi") return std::numbers::pi;
    if (name == "e") return std::numbers::e;

    if (!accept('(')) return fail("unknown identifier '" + std::string(name) + "'");
    auto arg = expr();
    if (!arg) return arg;
    if (!accept(')')) return fail("expected ')'");
    const double x = *arg;
    if (name == "sqrt") {
      if (x < 0) return fail("sqrt of negative number");
      return std::sqrt(x);
    }
    if (name == "abs") return std::fabs(x);
    if (name == "sin") return std::sin(x);
    if (name == "cos") return std::cos(x);
    if (name == "tan") return std::tan(x);
    if (name == "exp") return std::exp(x);
    if (name == "floor") return std::floor(x);
    if (name == "ceil") return std::ceil(x);
    if (name == "log" || name == "log10") {
      if (x <= 0) return fail("log of non-positive number");
      return name == "log" ? std::log(x) : std::log10(x);
    }
    return fail("unknown function '" + std::string(name) + "'");
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  int depth_ = 0;
};

}  // namespace

Result<double> evaluate_expression(std::string_view expression) {
  if (expression.size() > 4096) return make_error(ErrorCode::kInvalidArgument, "expression too long");
  return ExpressionParser(expression).parse();
}

void register_defaults(ToolRegistry& registry) {
  registry.add(
      FunctionDeclaration{
          "get_current_time",
          "Returns the current UTC date and time in ISO-8601 format and as a Unix timestamp.",
          json::object()},
      [](const json&) -> Result<json> {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
        return json{{"utc", buf}, {"unix", static_cast<std::int64_t>(t)}};
      });

  registry.add(
      FunctionDeclaration{
          "evaluate_expression",
          "Evaluates an arithmetic expression exactly as a calculator would. Supports + - * / % ^, "
          "parentheses, pi, e, sqrt, abs, sin, cos, tan, log, log10, exp, floor, ceil. Use this "
          "instead of doing arithmetic in your head.",
          json{{"type", "object"},
               {"properties",
                {{"expression", {{"type", "string"}, {"description", "e.g. '(2^10 - 24) / 3'"}}}}},
               {"required", {"expression"}}}},
      [](const json& args) -> Result<json> {
        if (!args.contains("expression") || !args["expression"].is_string()) {
          return make_error(ErrorCode::kInvalidArgument, "missing string argument 'expression'");
        }
        auto value = evaluate_expression(args["expression"].get<std::string>());
        if (!value) return std::unexpected(value.error());
        return json{{"value", *value}};
      });
}

}  // namespace builtin_tools

}  // namespace gemini
