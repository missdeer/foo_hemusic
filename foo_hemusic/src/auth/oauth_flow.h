#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "auth/device_info.h"

// OAuth message layer: request-body builders and response parsers for the
// Linux.do (provider="linuxdo") polling login flow. Mirrors HE-Music-Flutter
// `OnlineApiClient` (api.md sec.2). Pure data -- no HTTP, no Win32 -- so the
// wire contract is unit-testable in isolation; transport + polling live
// elsewhere.
//
// Flow: POST /v1/auth/session -> open url -> poll GET /v1/auth/status?state=
//       -> on success GET /v1/auth/result?state= -> token triple.

namespace hemusic {

// POST /v1/auth/session response.
struct AuthCodeUrlResult {
    std::string url;    // external authorize URL to open in the browser
    std::string state;  // opaque handle for status polling / result exchange
    int checkInterval = 0;  // suggested poll interval (seconds)
    long long expiresAt = 0;
};

// GET /v1/auth/status?state= status field (api.md sec.2.3).
enum class AuthStatus : std::uint8_t {
    Pending,
    Success,
    Failed,
    Expired,
    Error,
    Unknown
};

// GET /v1/auth/status?state= response.
struct AuthStatusResult {
    std::string status;  // raw status string from the backend
    std::string error;   // populated on status=="error"
    int checkInterval = 0;
    long long expiresAt = 0;

    AuthStatus kind() const { return classifyAuthStatus(status); }

    static AuthStatus classifyAuthStatus(std::string_view status);
};

// Token triple from /v1/auth/result, /v1/auth/token/refresh, etc. (api.md
// sec.0.6).
struct AuthTokenResult {
    std::string accessToken;
    std::string refreshToken;  // may be empty (refresh reuses the old one)
    long long expiresAt = 0;
};

// --- Request bodies ----------------------------------------------------------

// POST /v1/auth/session body. redirect_uri is included only when non-empty
// (Flutter omits it so the backend uses its configured default callback).
boost::json::object buildSessionRequest(const std::string& provider,
                                        const std::string& redirectUri,
                                        const DeviceInfo& device);

// POST /v1/auth/token/refresh body.
boost::json::object buildRefreshRequest(const std::string& refreshToken,
                                        const DeviceInfo& device);

// --- Response parsers --------------------------------------------------------

// GET /v1/auth/providers -> list of provider ids. Empty/missing -> {}.
std::vector<std::string> parseAuthProviders(const boost::json::value& body);

AuthCodeUrlResult parseAuthCodeUrl(const boost::json::value& body);
AuthStatusResult parseAuthStatus(const boost::json::value& body);

// Token triple with legacy fallback: when access_token is absent, accept a
// top-level "token" or nested "data.token" (api.md sec.0.6, Flutter
// _findToken).
AuthTokenResult parseAuthToken(const boost::json::value& body);

}  // namespace hemusic
