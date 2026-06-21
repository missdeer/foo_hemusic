#include "auth/login_flow.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <optional>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "net/json_codec.h"
#include "net/url_codec.h"

namespace hemusic {

namespace {

constexpr int kDefaultPollSeconds = 2;  // Flutter fallback when interval <= 0
constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;  // exclusive upper bound (2xx)

// Parses a response body into a JSON value; non-JSON / empty -> null value so
// the lenient json:: helpers degrade to defaults instead of throwing.
boost::json::value parseBody(const std::string& body) {
    boost::system::error_code ec;
    auto v = boost::json::parse(body, ec);
    if (ec) {
        return nullptr;
    }
    return v;
}

bool isHttpOk(const HttpResponse& resp) {
    return resp.ok && resp.status >= kHttpOkMin && resp.status < kHttpOkMax;
}

LoginResult terminal(LoginOutcome outcome, std::string message = {}) {
    LoginResult r;
    r.outcome = outcome;
    r.message = std::move(message);
    return r;
}

LoginResult transportFailure(const HttpResponse& resp) {
    return terminal(LoginOutcome::TransportError,
                    !resp.error.empty()
                        ? resp.error
                        : "HTTP " + std::to_string(resp.status));
}

long long nowEpochSeconds() {
    return static_cast<long long>(std::time(nullptr));
}

void notify(const LoginCallbacks& cb, LoginPhase phase) {
    if (cb.onProgress) {
        cb.onProgress(phase);
    }
}

HttpRequest jsonPost(std::string url, std::string body) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = std::move(url);
    req.body = std::move(body);
    return req;
}

HttpRequest get(std::string url) {
    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = std::move(url);
    return req;
}

// The authorize URL goes straight to ShellExecuteW (openUrl seam). Restrict it
// to http(s) so a malicious/MITM'd backend can't make us launch arbitrary
// protocol handlers (file:, custom schemes) from the configured base URL.
bool isHttpUrl(std::string_view url) {
    // prefix is lowercase; compare case-insensitively against url's head.
    auto startsWithCI = [](std::string_view s, std::string_view prefix) {
        return s.size() >= prefix.size() &&
               std::equal(
                   prefix.begin(), prefix.end(), s.begin(), [](char p, char c) {
                       return p == std::tolower(static_cast<unsigned char>(c));
                   });
    };
    return startsWithCI(url, "http://") || startsWithCI(url, "https://");
}

bool iequalsAscii(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Step 1: resolve the requested provider against the backend's advertised ids,
// which are case-sensitive on the wire (the HE-Music backend advertises
// "LinuxDo", and POST /v1/auth/session 404s on any other casing). Mirrors the
// Flutter client, which submits the id taken from /v1/auth/providers rather
// than a hard-coded string. Returns the backend's exact id, or nullopt + fills
// `out`.
std::optional<std::string> resolveProvider(std::string_view baseUrl,
                                           const std::string& requested,
                                           const LoginCallbacks& cb,
                                           LoginResult& out) {
    auto resp = cb.transport(get(buildAuthUrl(baseUrl, "/v1/auth/providers")));
    if (!isHttpOk(resp)) {
        out = transportFailure(resp);
        return std::nullopt;
    }
    auto providers = parseAuthProviders(parseBody(resp.body));
    for (auto& id : providers) {
        if (iequalsAscii(id, requested)) {
            return id;
        }
    }
    out = terminal(LoginOutcome::Failed,
                   "provider not offered by backend: " + requested);
    return std::nullopt;
}

// Step 2: POST /v1/auth/session, filling `out`. Returns a terminal result to
// abort with, or nullopt on success.
std::optional<LoginResult> beginSession(std::string_view baseUrl,
                                        const std::string& provider,
                                        const std::string& redirectUri,
                                        const DeviceInfo& device,
                                        const LoginCallbacks& cb,
                                        AuthCodeUrlResult& out) {
    auto body = boost::json::serialize(
        buildSessionRequest(provider, redirectUri, device));
    auto resp = cb.transport(
        jsonPost(buildAuthUrl(baseUrl, "/v1/auth/session"), std::move(body)));
    if (!isHttpOk(resp)) {
        return transportFailure(resp);
    }
    out = parseAuthCodeUrl(parseBody(resp.body));
    if (out.url.empty() || out.state.empty()) {
        return terminal(LoginOutcome::Failed,
                        "auth session returned no url/state");
    }
    return std::nullopt;
}

// On status==success: GET /v1/auth/result and validate the token triple.
LoginResult fetchResultToken(std::string_view baseUrl, const std::string& state,
                             const LoginCallbacks& cb) {
    auto resp = cb.transport(
        get(buildAuthUrl(baseUrl, "/v1/auth/result", {{"state", state}})));
    if (!isHttpOk(resp)) {
        return transportFailure(resp);
    }
    auto token = parseAuthToken(parseBody(resp.body));
    if (token.accessToken.empty()) {
        return terminal(LoginOutcome::Failed, "auth result returned no token");
    }
    LoginResult r;
    r.outcome = LoginOutcome::Success;
    r.token = std::move(token);
    return r;
}

// Step 4: poll /v1/auth/status until terminal. Mirrors Flutter: only
// success/failed/expired end the loop; everything else keeps waiting.
LoginResult pollUntilTerminal(std::string_view baseUrl,
                              const AuthCodeUrlResult& session,
                              const LoginCallbacks& cb) {
    long long expiresAt = session.expiresAt;  // epoch seconds, 0 == no deadline
    int interval =
        session.checkInterval > 0 ? session.checkInterval : kDefaultPollSeconds;
    const std::string statusUrl =
        buildAuthUrl(baseUrl, "/v1/auth/status", {{"state", session.state}});

    for (;;) {
        if (cb.wait(interval)) {
            return terminal(LoginOutcome::Cancelled);
        }
        if (expiresAt > 0 && nowEpochSeconds() > expiresAt) {
            return terminal(LoginOutcome::Expired);
        }

        auto resp = cb.transport(get(statusUrl));
        if (!isHttpOk(resp)) {
            return transportFailure(resp);
        }
        auto status = parseAuthStatus(parseBody(resp.body));
        if (status.expiresAt > 0) {
            expiresAt = status.expiresAt;
        }
        if (status.checkInterval > 0) {
            interval = status.checkInterval;
        }

        switch (status.kind()) {
            case AuthStatus::Success:
                notify(cb, LoginPhase::Finalizing);
                return fetchResultToken(baseUrl, session.state, cb);
            case AuthStatus::Failed:
                return terminal(LoginOutcome::Failed, status.error);
            case AuthStatus::Expired:
                return terminal(LoginOutcome::Expired);
            default:
                break;  // pending / error / unknown -> keep polling
        }
    }
}

}  // namespace

std::string buildAuthUrl(
    std::string_view baseUrl, std::string_view path,
    const std::vector<std::pair<std::string, std::string>>& query) {
    return url::buildUrl(baseUrl, path, query);
}

LoginResult runLogin(std::string_view baseUrl, const std::string& provider,
                     const std::string& redirectUri, const DeviceInfo& device,
                     const LoginCallbacks& cb) {
    notify(cb, LoginPhase::Connecting);
    LoginResult resolveFail;
    auto providerId = resolveProvider(baseUrl, provider, cb, resolveFail);
    if (!providerId) {
        return resolveFail;
    }
    AuthCodeUrlResult session;
    if (auto fail = beginSession(baseUrl, *providerId, redirectUri, device, cb,
                                 session)) {
        return *fail;
    }
    if (!isHttpUrl(session.url)) {
        return terminal(LoginOutcome::Failed, "untrusted authorize URL");
    }
    if (!cb.openUrl(session.url)) {
        return terminal(LoginOutcome::Failed, "failed to open browser");
    }
    notify(cb, LoginPhase::WaitingForAuthorization);
    return pollUntilTerminal(baseUrl, session, cb);
}

}  // namespace hemusic
