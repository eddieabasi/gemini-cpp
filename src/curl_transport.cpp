#include "gemini/transport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#ifndef GEMINI_CPP_VERSION
#define GEMINI_CPP_VERSION "dev"
#endif

namespace gemini {

namespace {

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
  CurlGlobal(const CurlGlobal&) = delete;
  CurlGlobal& operator=(const CurlGlobal&) = delete;
};

void ensure_curl_global() { static CurlGlobal global; }

struct EasyDeleter {
  void operator()(CURL* h) const noexcept { curl_easy_cleanup(h); }
};
struct SlistDeleter {
  void operator()(curl_slist* l) const noexcept { curl_slist_free_all(l); }
};
using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;
using HeaderList = std::unique_ptr<curl_slist, SlistDeleter>;

/// One easy handle per thread: curl_easy_reset() keeps the connection cache, so repeated
/// calls from the same thread reuse TLS connections.
CURL* thread_handle() {
  ensure_curl_global();
  thread_local EasyHandle handle{curl_easy_init()};
  return handle.get();
}

struct Context {
  CURL* curl = nullptr;
  HttpResponse* response = nullptr;
  const ChunkCallback* on_chunk = nullptr;
  bool aborted = false;
};

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
  auto* ctx = static_cast<Context*>(user);
  const std::size_t n = size * count;
  if (ctx->on_chunk != nullptr) {
    long status = 0;
    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && status < 300) {
      if (!(*ctx->on_chunk)(std::string_view(data, n))) {
        ctx->aborted = true;
        return 0;  // makes curl fail with CURLE_WRITE_ERROR
      }
      return n;
    }
  }
  ctx->response->body.append(data, n);
  return n;
}

std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) {
  auto* ctx = static_cast<Context*>(user);
  const std::size_t n = size * count;
  std::string_view line(data, n);
  if (line.starts_with("HTTP/")) {  // new response (redirect or 100-continue)
    ctx->response->headers.clear();
    return n;
  }
  const auto colon = line.find(':');
  if (colon == std::string_view::npos) return n;
  std::string name(line.substr(0, colon));
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string_view value = line.substr(colon + 1);
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
    value.remove_suffix(1);
  }
  ctx->response->headers[std::move(name)] = std::string(value);
  return n;
}

class CurlTransport final : public Transport {
 public:
  Result<HttpResponse> send(const HttpRequest& request) override { return perform(request, nullptr); }

  Result<HttpResponse> send_streaming(const HttpRequest& request,
                                      const ChunkCallback& on_chunk) override {
    return perform(request, &on_chunk);
  }

 private:
  static Result<HttpResponse> perform(const HttpRequest& request, const ChunkCallback* on_chunk) {
    CURL* curl = thread_handle();
    if (curl == nullptr) return make_error(ErrorCode::kTransport, "curl_easy_init failed");
    curl_easy_reset(curl);

    HttpResponse response;
    Context ctx{curl, &response, on_chunk, false};

    HeaderList headers;
    for (const auto& [name, value] : request.headers) {
      const std::string line = name + ": " + value;
      curl_slist* next = curl_slist_append(headers.get(), line.c_str());
      if (next == nullptr) return make_error(ErrorCode::kTransport, "curl_slist_append failed");
      (void)headers.release();
      headers.reset(next);
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "gemini-cpp/" GEMINI_CPP_VERSION);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // any encoding curl supports
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15'000L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    const long timeout_ms = static_cast<long>(request.timeout.count());
    if (on_chunk != nullptr) {
      // Stall detection instead of a total deadline: abort if < 1 byte/s for timeout seconds.
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, std::max(1L, timeout_ms / 1000));
    } else {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    }

    if (request.method == "GET") {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(request.body.size()));
      if (request.method != "POST") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
      }
    }

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);

    if (rc != CURLE_OK) {
      if (ctx.aborted) return make_error(ErrorCode::kCancelled, "request cancelled by caller");
      std::string message = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(rc);
      const ErrorCode code = (rc == CURLE_OPERATION_TIMEDOUT) ? ErrorCode::kTimeout
                                                              : ErrorCode::kTransport;
      return make_error(code, "curl: " + message);
    }
    return response;
  }
};

}  // namespace

std::unique_ptr<Transport> make_curl_transport() { return std::make_unique<CurlTransport>(); }

}  // namespace gemini
