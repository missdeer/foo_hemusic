#include "net/api_client.h"

#include <deque>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include "auth/device_info.h"
#include "auth/oauth_flow.h"
#include "net/http_client.h"

using hemusic::ApiClient;
using hemusic::AuthTokenResult;
using hemusic::DeviceInfo;
using hemusic::HttpMethod;
using hemusic::HttpRequest;
using hemusic::HttpResponse;
using hemusic::isAuthExcludedPath;
using hemusic::makeDeviceInfo;

namespace {

constexpr char kBaseUrl[] = "https://y.wjhe.top";

DeviceInfo sampleDevice() {
    return makeDeviceInfo("flutter_windows_abc", "MY-PC", "0.0.1");
}

HttpResponse resp(long status, std::string body = {}) {
    HttpResponse r;
    r.ok = true;
    r.status = status;
    r.body = std::move(body);
    return r;
}

HttpResponse transportFailure() {
    HttpResponse r;
    r.ok = false;
    r.error = "winhttp down";
    return r;
}

// Records every request and replays scripted responses front-to-back, so a test
// can assert exactly which exchanges the client made and in what order.
struct FakeTransport {
    std::vector<HttpRequest> seen;
    std::deque<HttpResponse> scripted;

    ApiClient::Transport fn() {
        return [this](const HttpRequest& req) -> HttpResponse {
            seen.push_back(req);
            if (scripted.empty()) {
                FAIL("transport called more times than scripted");
                return resp(599);
            }
            HttpResponse r = scripted.front();
            scripted.pop_front();
            return r;
        };
    }
};

bool isRefreshCall(const HttpRequest& req) {
    return req.method == HttpMethod::Post &&
           req.url.find("/v1/auth/token/refresh") != std::string::npos;
}

}  // namespace

TEST_CASE("isAuthExcludedPath matches login/refresh family, not data paths") {
    // Self-401 of the auth exchanges must never trigger another
    // refresh/redirect (api.md sec.0.4) -- that's the rule, so assert the whole
    // family.
    CHECK(isAuthExcludedPath("https://y.wjhe.top/v1/user/login"));
    CHECK(isAuthExcludedPath("https://y.wjhe.top/v1/auth/token/refresh"));
    CHECK(isAuthExcludedPath("https://y.wjhe.top/v1/auth/result?state=x"));
    CHECK(isAuthExcludedPath("https://y.wjhe.top/v1/auth/qr/result?state=x"));
    CHECK(isAuthExcludedPath("https://y.wjhe.top/v1/auth/logout"));

    CHECK_FALSE(isAuthExcludedPath("https://y.wjhe.top/v1/page/discover"));
    CHECK_FALSE(isAuthExcludedPath("https://y.wjhe.top/v1/search?keyword=a"));
    CHECK_FALSE(isAuthExcludedPath("https://y.wjhe.top/v1/song/url?id=1"));

    // Only the path is matched: an excluded word inside the query string must
    // NOT exclude an otherwise-eligible request (else a 401 silently skips
    // refresh).
    CHECK_FALSE(
        isAuthExcludedPath("https://y.wjhe.top/v1/page/discover?next=/login"));
    CHECK_FALSE(
        isAuthExcludedPath("https://y.wjhe.top/v1/search?q=auth/result&p=1"));
}

TEST_CASE("send injects the current access token and passes 2xx through") {
    FakeTransport t;
    t.scripted.push_back(resp(200, R"({"ok":true})"));

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "RT1", 100);

    HttpRequest req;
    req.url = "https://y.wjhe.top/v1/page/discover";
    HttpResponse r = client.send(req);

    CHECK(r.status == 200);
    REQUIRE(t.seen.size() == 1);
    // The client owns auth: the bearer carried is the cached access token.
    CHECK(t.seen.at(0).bearerToken == "AT1");
}

TEST_CASE(
    "401 on an eligible path refreshes once and replays with the new token") {
    FakeTransport t;
    t.scripted.push_back(resp(401));  // original request
    t.scripted.push_back(resp(
        200,
        R"({"access_token":"AT2","refresh_token":"RT2","expires_at":222})"));
    t.scripted.push_back(resp(200, R"({"data":1})"));  // replay

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "RT1", 111);

    AuthTokenResult refreshed;
    bool fired = false;
    client.setOnTokensRefreshed([&](const AuthTokenResult& tr) {
        refreshed = tr;
        fired = true;
    });

    HttpRequest req;
    req.url = "https://y.wjhe.top/v1/page/discover";
    HttpResponse r = client.send(req);

    // Replayed response is what the caller sees.
    CHECK(r.status == 200);
    CHECK(r.body == R"({"data":1})");

    REQUIRE(t.seen.size() == 3);
    // 1: original carried the old token...
    CHECK(t.seen.at(0).bearerToken == "AT1");
    // 2: ...then a refresh POST hit the refresh endpoint with the refresh
    // token.
    CHECK(isRefreshCall(t.seen.at(1)));
    auto body = boost::json::parse(t.seen.at(1).body).as_object();
    CHECK(body.at("refresh_token").as_string() == "RT1");
    // 3: replay carried the *new* access token.
    CHECK(t.seen.at(2).bearerToken == "AT2");
    CHECK(t.seen.at(2).url == req.url);

    // Cached triple advanced + caller was notified to persist it.
    CHECK(client.accessToken() == "AT2");
    CHECK(client.refreshToken() == "RT2");
    CHECK(client.expiresAt() == 222);
    REQUIRE(fired);
    CHECK(refreshed.accessToken == "AT2");
    CHECK(refreshed.refreshToken == "RT2");
}

TEST_CASE("401 on an excluded path does not refresh") {
    FakeTransport t;
    t.scripted.push_back(resp(401));

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "RT1", 1);

    HttpRequest req;
    req.url =
        "https://y.wjhe.top/v1/user/login";  // matches the exclusion regex
    HttpResponse r = client.send(req);

    CHECK(r.status == 401);
    CHECK(t.seen.size() == 1);  // no refresh attempt
    CHECK(client.accessToken() == "AT1");
}

TEST_CASE("401 with no refresh token surfaces the 401 unchanged") {
    FakeTransport t;
    t.scripted.push_back(resp(401));

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "", 0);  // logged in but no refresh credential

    HttpRequest req;
    req.url = "https://y.wjhe.top/v1/page/discover";
    HttpResponse r = client.send(req);

    CHECK(r.status == 401);
    CHECK(t.seen.size() == 1);
}

TEST_CASE(
    "refresh failure leaves the cached token intact and returns the 401") {
    SECTION("refresh endpoint itself returns non-2xx") {
        FakeTransport t;
        t.scripted.push_back(resp(401));  // original
        t.scripted.push_back(resp(401));  // refresh rejected

        ApiClient client(t.fn(), kBaseUrl, sampleDevice());
        client.setTokens("AT1", "RT1", 9);

        HttpRequest req;
        req.url = "https://y.wjhe.top/v1/page/discover";
        HttpResponse r = client.send(req);

        CHECK(r.status == 401);     // original 401 surfaced, no replay
        CHECK(t.seen.size() == 2);  // original + refresh, no replay
        CHECK(client.accessToken() == "AT1");
        CHECK(client.refreshToken() == "RT1");
    }

    SECTION("refresh transport fails") {
        FakeTransport t;
        t.scripted.push_back(resp(401));
        t.scripted.push_back(transportFailure());

        ApiClient client(t.fn(), kBaseUrl, sampleDevice());
        client.setTokens("AT1", "RT1", 9);

        HttpRequest req;
        req.url = "https://y.wjhe.top/v1/page/discover";
        HttpResponse r = client.send(req);

        CHECK(r.status == 401);
        CHECK(t.seen.size() == 2);
        CHECK(client.accessToken() == "AT1");
    }

    SECTION("refresh 200 but empty access_token is a failure") {
        FakeTransport t;
        t.scripted.push_back(resp(401));
        t.scripted.push_back(resp(200, R"({"refresh_token":"RTx"})"));

        ApiClient client(t.fn(), kBaseUrl, sampleDevice());
        client.setTokens("AT1", "RT1", 9);

        HttpRequest req;
        req.url = "https://y.wjhe.top/v1/page/discover";
        HttpResponse r = client.send(req);

        CHECK(r.status == 401);
        CHECK(t.seen.size() == 2);  // no replay on a useless refresh
        CHECK(client.accessToken() == "AT1");
        CHECK(client.refreshToken() == "RT1");
    }
}

TEST_CASE("refresh response without a new refresh token retains the old one") {
    // api.md sec.6: the refresh token is retained when the server omits it --
    // the long-lived credential must not be wiped on a non-rotating refresh.
    FakeTransport t;
    t.scripted.push_back(resp(401));
    t.scripted.push_back(resp(200, R"({"access_token":"AT2","expires_at":5})"));
    t.scripted.push_back(resp(200, R"({"ok":1})"));

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "RT1", 1);

    HttpRequest req;
    req.url = "https://y.wjhe.top/v1/page/discover";
    HttpResponse r = client.send(req);

    CHECK(r.status == 200);
    CHECK(client.accessToken() == "AT2");
    CHECK(client.refreshToken() == "RT1");  // retained, not cleared
}

TEST_CASE("the replay is attempted only once even if it also 401s") {
    // Guards against an infinite refresh loop: a replay that still 401s is the
    // terminal answer, not the trigger for another refresh.
    FakeTransport t;
    t.scripted.push_back(resp(401));  // original
    t.scripted.push_back(resp(
        200, R"({"access_token":"AT2","refresh_token":"RT2"})"));  // refresh
    t.scripted.push_back(resp(401));  // replay still unauthorized

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "RT1", 1);

    HttpRequest req;
    req.url = "https://y.wjhe.top/v1/page/discover";
    HttpResponse r = client.send(req);

    CHECK(r.status == 401);
    CHECK(t.seen.size() == 3);  // original + refresh + single replay, then stop
}

TEST_CASE("non-401 errors pass through without a refresh") {
    FakeTransport t;
    t.scripted.push_back(resp(500, R"({"error":"boom"})"));

    ApiClient client(t.fn(), kBaseUrl, sampleDevice());
    client.setTokens("AT1", "RT1", 1);

    HttpRequest req;
    req.url = "https://y.wjhe.top/v1/page/discover";
    HttpResponse r = client.send(req);

    CHECK(r.status == 500);
    CHECK(t.seen.size() == 1);
    CHECK(client.accessToken() == "AT1");
}
