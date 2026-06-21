#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "auth/device_info.h"
#include "auth/oauth_flow.h"
#include "net/http_client.h"

// OAuth login orchestration: drives the polling login flow end to end on top of
// the pure message layer (oauth_flow) and the WinHTTP transport (http_client).
// Mirrors HE-Music-Flutter login_page `_pollAuthStatus` (api.md sec.2):
//
//   GET  /v1/auth/providers          (verify provider is offered)
//   POST /v1/auth/session            -> { url, state, check_interval,
//   expires_at } openUrl(url)                     (system browser -> Linux.do
//   authorize) loop every check_interval s:
//     GET /v1/auth/status?state=     -> pending | success | failed | expired
//       success -> GET /v1/auth/result?state= -> token triple
//
// Side effects are injected (transport / openUrl / wait) so the whole loop is
// unit-testable with fakes and the file stays free of the foobar2000 SDK.

namespace hemusic {

enum class LoginOutcome : std::uint8_t {
    Success,        // token holds the access/refresh pair
    Cancelled,      // caller-requested abort during the flow
    Expired,        // backend or local deadline reached before success
    Failed,         // backend reported status=="failed" / bad response
    TransportError  // network failure or non-2xx HTTP response
};

struct LoginResult {
    LoginOutcome outcome = LoginOutcome::TransportError;
    AuthTokenResult token;  // populated only when outcome == Success
    std::string message;  // human-readable detail (error text / failure cause)
};

// Sends one HTTP exchange. Real impl forwards to HttpClient::send; tests inject
// a fake. baseUrl + paths are already absolute in the request.
using TransportFn = std::function<HttpResponse(const HttpRequest&)>;

// Opens the authorize URL in the system browser (ShellExecuteW). Returns false
// when the browser could not be launched.
using OpenUrlFn = std::function<bool(const std::string& url)>;

// Waits up to `seconds` between status polls. Returns true if the flow was
// cancelled during the wait (real impl: WaitForSingleObjectEx on a cancel
// event), in which case the loop aborts immediately.
using WaitFn = std::function<bool(int seconds)>;

// Coarse phase of the (blocking) flow, so a worker-thread caller can drive a
// "waiting for authorization" UI without polling internal state.
enum class LoginPhase : std::uint8_t {
    Connecting,               // contacting the backend (providers + session)
    WaitingForAuthorization,  // browser opened, polling /v1/auth/status
    Finalizing                // status==success, exchanging for the token
};

// Optional progress sink, invoked on the calling thread at each phase change.
using ProgressFn = std::function<void(LoginPhase)>;

struct LoginCallbacks {
    TransportFn transport;
    OpenUrlFn openUrl;
    WaitFn wait;
    ProgressFn onProgress;  // optional; may be empty
};

// Joins baseUrl (trailing '/' trimmed) + path + percent-encoded query (via
// net/url_codec). Exposed for unit testing the URL/query assembly.
std::string buildAuthUrl(
    std::string_view baseUrl, std::string_view path,
    const std::vector<std::pair<std::string, std::string>>& query = {});

// Runs the full login flow. provider is "linuxdo"; redirectUri is normally ""
// (the backend uses its configured default callback). Blocks until a terminal
// outcome; cancellation is observed via cb.wait returning true.
LoginResult runLogin(std::string_view baseUrl, const std::string& provider,
                     const std::string& redirectUri, const DeviceInfo& device,
                     const LoginCallbacks& cb);

}  // namespace hemusic
