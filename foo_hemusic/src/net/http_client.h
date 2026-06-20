#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Synchronous WinHTTP transport for the HE-Music backend. Phase 1 needs only
// blocking GET/POST with a JSON body and a "authorization: Bearer" header; the
// async variant + interceptor chain arrive in Phase 2 (PLAN sec.5). Kept free
// of the foobar2000 SDK so it can live in hemusic_core alongside the unit
// tests.

namespace hemusic {

// Default timeouts (PLAN sec.5.1: connect 20s / read 30s).
inline constexpr int kDefaultConnectTimeoutMs = 20000;
inline constexpr int kDefaultReadTimeoutMs = 30000;

enum class HttpMethod : std::uint8_t { Get, Post };

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;  // absolute http(s) URL
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;         // request payload (sent for POST); e.g. JSON
    std::string bearerToken;  // when non-empty -> "authorization: Bearer <t>"
    int connectTimeoutMs = kDefaultConnectTimeoutMs;
    int readTimeoutMs = kDefaultReadTimeoutMs;
};

struct HttpResponse {
    bool ok = false;   // true when the exchange completed (for any HTTP status)
    long status = 0;   // HTTP status code when ok
    std::string body;  // response body when ok
    unsigned long winError = 0;  // WinHTTP/Win32 error code when !ok
    std::string error;           // human-readable transport failure when !ok
};

// Components of an absolute http(s) URL (WinHttpCrackUrl). port is filled with
// the scheme default (443/80) when the URL omits it; path always begins "/".
struct ParsedUrl {
    bool https = true;
    std::wstring host;
    int port = 0;
    std::wstring path;  // path + query
};

std::optional<ParsedUrl> crackUrl(const std::string& url);

// Owns a reusable WinHTTP session handle; one instance serves many sequential
// requests. Not thread-safe: callers must not invoke send() concurrently on the
// same instance.
class HttpClient {
   public:
    explicit HttpClient(const std::wstring& userAgent = L"foo_hemusic");
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    HttpResponse send(const HttpRequest& req);

   private:
    void* session_ = nullptr;  // HINTERNET
};

}  // namespace hemusic
