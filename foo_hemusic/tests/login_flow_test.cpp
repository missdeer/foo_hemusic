#include "auth/login_flow.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "auth/device_info.h"
#include "net/http_client.h"

using hemusic::AuthStatus;
using hemusic::buildAuthUrl;
using hemusic::DeviceInfo;
using hemusic::HttpRequest;
using hemusic::HttpResponse;
using hemusic::LoginCallbacks;
using hemusic::LoginOutcome;
using hemusic::LoginPhase;
using hemusic::makeDeviceInfo;
using hemusic::percentEncode;
using hemusic::runLogin;

namespace {

DeviceInfo sampleDevice() {
    return makeDeviceInfo("flutter_windows_abc", "MY-PC", "0.0.1");
}

constexpr long kHttpOk = 200;

HttpResponse ok(std::string body) {
    HttpResponse r;
    r.ok = true;
    r.status = kHttpOk;
    r.body = std::move(body);
    return r;
}

bool isPath(const std::string& url, std::string_view path) {
    auto pos = url.find(path);
    if (pos == std::string::npos) {
        return false;
    }
    // ensure it's the path segment, not a query value
    const auto end = pos + path.size();
    const char after = end < url.size() ? url.at(end) : '\0';
    return after == '\0' || after == '?';
}

// Scriptable transport: providers + session are fixed; status returns the next
// scripted body each call, so a test drives the poll state machine turn by
// turn.
struct FakeBackend {
    std::string providersBody = R"({"list":["linuxdo"]})";
    std::string sessionBody =
        R"({"url":"https://linux.do/oauth?x=1","state":"st 7","check_interval":1,"expires_at":0})";
    std::string resultBody =
        R"({"access_token":"AT","refresh_token":"RT","expires_at":99})";
    std::vector<std::string> statusScript;  // consumed front-to-back
    size_t statusIdx = 0;
    std::vector<HttpRequest> seen;

    LoginCallbacks callbacks(bool& opened, int& waits) {
        LoginCallbacks cb;
        cb.transport = [this](const HttpRequest& req) -> HttpResponse {
            seen.push_back(req);
            if (isPath(req.url, "/v1/auth/providers")) {
                return ok(providersBody);
            }
            if (isPath(req.url, "/v1/auth/session")) {
                return ok(sessionBody);
            }
            if (isPath(req.url, "/v1/auth/status")) {
                const auto& body = statusScript.at(statusIdx);
                if (statusIdx + 1 < statusScript.size()) {
                    ++statusIdx;
                }
                return ok(body);
            }
            if (isPath(req.url, "/v1/auth/result")) {
                return ok(resultBody);
            }
            HttpResponse r;
            r.error = "unexpected url: " + req.url;
            return r;
        };
        cb.openUrl = [&opened](const std::string&) {
            opened = true;
            return true;
        };
        cb.wait = [&waits](int) {
            ++waits;
            return false;  // never cancelled
        };
        return cb;
    }
};

}  // namespace

TEST_CASE("percentEncode keeps unreserved, escapes the rest", "[login]") {
    REQUIRE(percentEncode("abcXYZ-_.~09") == "abcXYZ-_.~09");
    // space and '/' must be escaped so opaque state survives as a query value.
    REQUIRE(percentEncode("a b/c") == "a%20b%2Fc");
}

TEST_CASE("buildAuthUrl trims base slash and appends encoded query",
          "[login]") {
    REQUIRE(buildAuthUrl("https://api.test/", "/v1/auth/status",
                         {{"state", "st 7"}}) ==
            "https://api.test/v1/auth/status?state=st%207");
    REQUIRE(buildAuthUrl("https://api.test", "/v1/auth/providers") ==
            "https://api.test/v1/auth/providers");
}

TEST_CASE("runLogin success: pending then success yields token", "[login]") {
    FakeBackend be;
    be.statusScript = {R"({"status":"pending"})", R"({"status":"success"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Success);
    REQUIRE(res.token.accessToken == "AT");
    REQUIRE(res.token.refreshToken == "RT");
    REQUIRE(res.token.expiresAt == 99);
    REQUIRE(opened);
    // one wait before each of the two status polls.
    REQUIRE(waits == 2);
    // providers (resolve) -> session -> status.
    REQUIRE(isPath(be.seen.at(0).url, "/v1/auth/providers"));
    REQUIRE(isPath(be.seen.at(1).url, "/v1/auth/session"));
    REQUIRE(isPath(be.seen.at(2).url, "/v1/auth/status"));
}

// The backend advertises provider ids case-sensitively ("LinuxDo") and 404s on
// any other casing; runLogin must resolve our "linuxdo" request to the exact
// id.
TEST_CASE("runLogin resolves provider id case-insensitively", "[login]") {
    FakeBackend be;
    be.providersBody = R"({"list":["LinuxDo"]})";
    be.statusScript = {R"({"status":"success"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Success);  // matched despite casing
    REQUIRE(opened);
}

TEST_CASE("runLogin fails when the backend offers no matching provider",
          "[login]") {
    FakeBackend be;
    be.providersBody = R"({"list":["github"]})";
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Failed);
    REQUIRE_FALSE(opened);  // never reached the session / browser hop
}

TEST_CASE("runLogin surfaces backend status=failed with its error", "[login]") {
    FakeBackend be;
    be.statusScript = {R"({"status":"failed","error":"denied by user"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Failed);
    REQUIRE(res.message == "denied by user");
}

TEST_CASE("runLogin reports status=expired", "[login]") {
    FakeBackend be;
    be.statusScript = {R"({"status":"expired"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Expired);
}

TEST_CASE("runLogin honors cancellation during the poll wait", "[login]") {
    FakeBackend be;
    be.statusScript = {R"({"status":"pending"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);
    // override wait to cancel on the first call.
    cb.wait = [](int) { return true; };

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Cancelled);
    REQUIRE(opened);  // browser was opened before the (cancelled) poll
}

// onProgress drives the UI's "waiting for authorization" text: it must fire
// Connecting -> WaitingForAuthorization -> Finalizing across a successful
// login.
TEST_CASE("runLogin reports phase transitions via onProgress", "[login]") {
    FakeBackend be;
    be.statusScript = {R"({"status":"pending"})", R"({"status":"success"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);
    std::vector<LoginPhase> phases;
    cb.onProgress = [&phases](LoginPhase p) { phases.push_back(p); };

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Success);
    REQUIRE(phases ==
            std::vector<LoginPhase>{LoginPhase::Connecting,
                                    LoginPhase::WaitingForAuthorization,
                                    LoginPhase::Finalizing});
}

// Defense at the shell boundary: a backend returning a non-http(s) authorize
// URL must be rejected before it reaches ShellExecuteW (openUrl).
TEST_CASE("runLogin rejects a non-http(s) authorize URL", "[login]") {
    FakeBackend be;
    be.sessionBody =
        R"({"url":"file:///C:/Windows/System32/calc.exe","state":"st","check_interval":1})";
    be.statusScript = {R"({"status":"success"})"};
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::Failed);
    REQUIRE(res.message == "untrusted authorize URL");
    REQUIRE_FALSE(opened);  // never handed to the shell
}

TEST_CASE("runLogin maps transport failure to TransportError", "[login]") {
    FakeBackend be;
    bool opened = false;
    int waits = 0;
    auto cb = be.callbacks(opened, waits);
    cb.transport = [](const HttpRequest&) {
        HttpResponse r;
        r.ok = false;
        r.error = "winhttp boom";
        return r;
    };

    auto res = runLogin("https://api.test", "linuxdo", "", sampleDevice(), cb);

    REQUIRE(res.outcome == LoginOutcome::TransportError);
    REQUIRE(res.message == "winhttp boom");
}
