#include "net/api_client.h"

#include <regex>

#include <boost/json.hpp>

namespace hemusic {
namespace {

constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;  // exclusive upper bound (2xx)
constexpr long kHttpUnauthorized = 401;

// Non-JSON / empty body -> null value so the lenient json:: helpers in
// parseAuthToken degrade to defaults instead of throwing.
boost::json::value parseBody(const std::string& body) {
    boost::system::error_code ec;
    auto v = boost::json::parse(body, ec);
    if (ec) {
        return nullptr;
    }
    return v;
}

std::string trimTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool isHttpOk(const HttpResponse& resp) {
    return resp.ok && resp.status >= kHttpOkMin && resp.status < kHttpOkMax;
}

}  // namespace

bool isAuthExcludedPath(const std::string& url) {
    // Match only the path, like Flutter's `requestOptions.path`: strip the
    // query and the scheme+host first, so a value such as
    // ".../discover?next=/login" can't false-match and silently suppress a
    // refresh.
    std::string path = url;
    if (const auto q = path.find('?'); q != std::string::npos) {
        path.erase(q);
    }
    if (const auto s = path.find("://"); s != std::string::npos) {
        const auto slash = path.find('/', s + 3);
        path = slash == std::string::npos ? std::string{} : path.substr(slash);
    }
    static const std::regex re(
        R"(/(login|token/refresh|auth/result|auth/qr/result|auth/logout)\b)");
    return std::regex_search(path, re);
}

ApiClient::ApiClient(Transport transport, std::string baseUrl,
                     DeviceInfo device)
    : transport_(std::move(transport)),
      baseUrl_(trimTrailingSlash(std::move(baseUrl))),
      device_(std::move(device)) {}

void ApiClient::setTokens(std::string accessToken, std::string refreshToken,
                          long long expiresAt) {
    accessToken_ = std::move(accessToken);
    refreshToken_ = std::move(refreshToken);
    expiresAt_ = expiresAt;
}

HttpResponse ApiClient::send(HttpRequest req) {
    req.bearerToken = accessToken_;
    HttpResponse resp = transport_(req);

    // Only a real 401 from an eligible path with a usable refresh token is
    // worth refreshing for; anything else (transport failure, other status,
    // excluded path, no refresh token) passes through untouched.
    if (!resp.ok || resp.status != kHttpUnauthorized) {
        return resp;
    }
    if (isAuthExcludedPath(req.url) || refreshToken_.empty()) {
        return resp;
    }

    if (!refreshTokens(req.connectTimeoutMs, req.readTimeoutMs)) {
        return resp;  // refresh failed -> surface the original 401
    }

    req.bearerToken = accessToken_;  // updated by refreshTokens()
    return transport_(req);          // single replay; never retried again
}

std::optional<AuthTokenResult> ApiClient::refreshTokens(int connectTimeoutMs,
                                                        int readTimeoutMs) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = baseUrl_ + "/v1/auth/token/refresh";
    req.connectTimeoutMs = connectTimeoutMs;
    req.readTimeoutMs = readTimeoutMs;
    req.body =
        boost::json::serialize(buildRefreshRequest(refreshToken_, device_));
    // No bearer on the refresh exchange itself (matches Flutter's separate
    // refreshDio); the path is excluded from refresh anyway.

    HttpResponse resp = transport_(req);
    if (!isHttpOk(resp)) {
        return std::nullopt;
    }

    AuthTokenResult t = parseAuthToken(parseBody(resp.body));
    if (t.accessToken.empty()) {
        return std::nullopt;
    }

    // refresh_token empty -> keep the old one (api.md sec.6: the long-lived
    // credential is retained when the server omits it; Flutter
    // effectiveRefresh).
    accessToken_ = t.accessToken;
    if (!t.refreshToken.empty()) {
        refreshToken_ = t.refreshToken;
    }
    expiresAt_ = t.expiresAt;

    AuthTokenResult effective{accessToken_, refreshToken_, expiresAt_};
    if (onTokensRefreshed_) {
        onTokensRefreshed_(effective);
    }
    return effective;
}

}  // namespace hemusic
