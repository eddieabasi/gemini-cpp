#include "gemini/error.hpp"

namespace gemini {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kTransport: return "transport";
    case ErrorCode::kTimeout: return "timeout";
    case ErrorCode::kCancelled: return "cancelled";
    case ErrorCode::kInvalidArgument: return "invalid_argument";
    case ErrorCode::kUnauthenticated: return "unauthenticated";
    case ErrorCode::kNotFound: return "not_found";
    case ErrorCode::kRateLimited: return "rate_limited";
    case ErrorCode::kServer: return "server";
    case ErrorCode::kHttp: return "http";
    case ErrorCode::kParse: return "parse";
    case ErrorCode::kBlocked: return "blocked";
    case ErrorCode::kConfig: return "config";
    case ErrorCode::kIo: return "io";
  }
  return "unknown";
}

bool Error::retryable() const noexcept {
  switch (code) {
    case ErrorCode::kTransport:
    case ErrorCode::kTimeout:
    case ErrorCode::kRateLimited:
    case ErrorCode::kServer:
      return true;
    default:
      return false;
  }
}

std::string Error::describe() const {
  std::string out = "[";
  out += to_string(code);
  out += "] ";
  out += message;
  if (http_status != 0 || !api_status.empty()) {
    out += " (";
    if (http_status != 0) out += "HTTP " + std::to_string(http_status);
    if (!api_status.empty()) {
      if (http_status != 0) out += ' ';
      out += api_status;
    }
    out += ')';
  }
  return out;
}

}  // namespace gemini
