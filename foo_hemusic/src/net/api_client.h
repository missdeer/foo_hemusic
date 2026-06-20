#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "auth/device_info.h"
#include "auth/oauth_flow.h"
#include "net/http_client.h"

// Authenticated API transport: injects the bearer token on every request and,
// on a 401, transparently refreshes via POST /v1/auth/token/refresh and replays
// the original request once. Mirrors HE-Music-Flutter
// `TokenRefreshInterceptor` (core/network/token_refresh_interceptor.dart,
// api.md sec.0.4 / sec.6).
//
// Pure logic over an injected transport (std::function), so the refresh-and-
// replay contract is unit-testable with fakes and the file stays free of the
// foobar2000 SDK (lives in hemusic_core). The typed api/ clients call send().
//
// Out of scope here (deferred per PLAN): the UnauthorizedRedirect (clear token
// -> login UI) and ErrorMessage (toast) interceptors are SDK/UI-bound and land
// with UI wiring; the captcha challenge has its own Phase 6.

namespace hemusic {

// True when the request URL's path must NOT trigger token refresh or login
// redirect. Mirrors the Flutter exclusion regex
// /(login|token/refresh|auth/result|auth/qr/result|auth/logout)\b so the login
// and refresh exchanges themselves can't loop on their own 401s (api.md
// sec.0.4).
bool isAuthExcludedPath(const std::string& url);

class ApiClient {
   public:
    using Transport = std::function<HttpResponse(const HttpRequest&)>;

    // Invoked after a successful refresh with the effective token triple
    // (new access token + new-or-retained refresh token), so the caller can
    // persist it (TokenStore) and update any cached copy it holds.
    using TokensRefreshedFn = std::function<void(const AuthTokenResult&)>;

    ApiClient(Transport transport, std::string baseUrl, DeviceInfo device);

    // Replaces the cached token triple (e.g. after login or from TokenStore).
    void setTokens(std::string accessToken, std::string refreshToken,
                   long long expiresAt = 0);

    const std::string& accessToken() const { return accessToken_; }
    const std::string& refreshToken() const { return refreshToken_; }
    long long expiresAt() const { return expiresAt_; }

    void setOnTokensRefreshed(TokensRefreshedFn cb) {
        onTokensRefreshed_ = std::move(cb);
    }

    // Sends `req` with the current access token as the bearer (any bearerToken
    // already on `req` is overwritten -- this client owns auth). On a 401 from
    // an eligible path with a refresh token present, refreshes once and replays
    // the request with the new token, returning the replayed response; the
    // replay is never retried again, so a still-401 replay surfaces as-is.
    HttpResponse send(HttpRequest req);

   private:
    // Performs POST /v1/auth/token/refresh; on success updates the cached
    // triple (retaining the old refresh token when the response omits a new
    // one) and fires onTokensRefreshed. Returns the effective triple, or
    // nullopt on any transport/HTTP/parse failure or an empty access token.
    std::optional<AuthTokenResult> refreshTokens();

    Transport transport_;
    std::string baseUrl_;  // trailing '/' trimmed
    DeviceInfo device_;
    std::string accessToken_;
    std::string refreshToken_;
    long long expiresAt_ = 0;
    TokensRefreshedFn onTokensRefreshed_;
};

}  // namespace hemusic
