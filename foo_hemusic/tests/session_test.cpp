#include "core/session.h"

#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include "auth/device_info.h"
#include "auth/oauth_flow.h"
#include "auth/token_store.h"
#include "net/api_client.h"
#include "net/http_client.h"

using hemusic::ApiClient;
using hemusic::AuthTokenResult;
using hemusic::DeviceInfo;
using hemusic::HttpMethod;
using hemusic::HttpRequest;
using hemusic::HttpResponse;
using hemusic::makeDeviceInfo;
using hemusic::Session;
using hemusic::TokenStore;

namespace {

constexpr char kBaseUrl[] = "https://example.test";
constexpr long kStatusOk = 200;
constexpr long kStatusUnauthorized = 401;

DeviceInfo sampleDevice() {
    return makeDeviceInfo("flutter_windows_test", "TESTBOX", "0.0.1");
}

AuthTokenResult tokens(std::string access, std::string refresh,
                       long long expires) {
    AuthTokenResult t;
    t.accessToken = std::move(access);
    t.refreshToken = std::move(refresh);
    t.expiresAt = expires;
    return t;
}

HttpResponse resp(long status, std::string body = {}) {
    HttpResponse r;
    r.ok = true;
    r.status = status;
    r.body = std::move(body);
    return r;
}

// Replays scripted responses front-to-back, recording every request so a
// test can assert which exchanges happened in what order.
struct FakeTransport {
    std::vector<HttpRequest> seen;
    std::deque<HttpResponse> scripted;

    ApiClient::Transport fn() {
        return [this](const HttpRequest& req) -> HttpResponse {
            seen.push_back(req);
            REQUIRE(!scripted.empty());
            HttpResponse r = scripted.front();
            scripted.pop_front();
            return r;
        };
    }
};

std::filesystem::path freshTempPath() {
    auto base = std::filesystem::temp_directory_path();
    static int counter = 0;
    ++counter;
    return base /
           ("foo_hemusic_session_test_" + std::to_string(counter) + ".bin");
}

// Removes its auth listener on scope exit -- including stack unwinding from a
// failing REQUIRE -- so a leaked callback can't survive in the process-wide
// Session singleton and fire (reading freed test locals) during a later test.
// Constructed after the locals it captures, so it is destroyed before them.
struct ScopedAuthListener {
    Session::AuthListenerId id;
    explicit ScopedAuthListener(Session::AuthListener cb)
        : id(Session::instance().addAuthListener(std::move(cb))) {}
    ~ScopedAuthListener() { Session::instance().removeAuthListener(id); }
    ScopedAuthListener(const ScopedAuthListener&) = delete;
    ScopedAuthListener(ScopedAuthListener&&) = delete;
    ScopedAuthListener& operator=(const ScopedAuthListener&) = delete;
    ScopedAuthListener& operator=(ScopedAuthListener&&) = delete;
};

// Returns the session to a known empty + initialized state: there's no real
// singleton reset, so we clear -> re-init to a fresh path -> clear again so
// no prior credential leaks across tests.
void resetSession(const std::filesystem::path& path) {
    Session::instance().clearTokens();
    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    Session::instance().clearTokens();
}

}  // namespace

TEST_CASE("initialize loads previously persisted credential") {
    auto path = freshTempPath();
    {
        TokenStore seed(path);
        REQUIRE(seed.save(tokens("seedAT", "seedRT", 12345)));
    }

    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    REQUIRE(Session::instance().isInitialized());
    REQUIRE(Session::instance().isAuthenticated());
    auto t = Session::instance().currentTokens();
    REQUIRE(t.has_value());
    CHECK(t->accessToken == "seedAT");
    CHECK(t->refreshToken == "seedRT");
    CHECK(t->expiresAt == 12345);

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("initialize with no file leaves session authenticated=false") {
    resetSession(freshTempPath());
    CHECK(Session::instance().isInitialized());
    CHECK_FALSE(Session::instance().isAuthenticated());
    CHECK_FALSE(Session::instance().currentTokens().has_value());
}

TEST_CASE("setTokens persists and survives re-initialize from same path") {
    auto path = freshTempPath();
    Session::instance().initialize(path, kBaseUrl, sampleDevice());

    REQUIRE(Session::instance().setTokens(tokens("AT-x", "RT-x", 999)));
    CHECK(Session::instance().isAuthenticated());

    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    auto t = Session::instance().currentTokens();
    REQUIRE(t.has_value());
    CHECK(t->accessToken == "AT-x");
    CHECK(t->expiresAt == 999);

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("clearTokens removes file and forgets credential") {
    auto path = freshTempPath();
    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    REQUIRE(Session::instance().setTokens(tokens("AT", "RT", 1)));
    REQUIRE(std::filesystem::exists(path));

    Session::instance().clearTokens();
    CHECK_FALSE(Session::instance().isAuthenticated());
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("buildClient injects current bearer + base URL") {
    auto path = freshTempPath();
    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    REQUIRE(Session::instance().setTokens(tokens("AT-build", "RT-build", 0)));

    FakeTransport t;
    t.scripted.push_back(resp(kStatusOk, "{}"));

    auto client = Session::instance().buildClient(t.fn());
    REQUIRE(client.has_value());

    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = std::string(kBaseUrl) + "/v1/user/info";
    auto r = client->send(req);
    CHECK(r.status == 200);
    REQUIRE(t.seen.size() == 1);
    CHECK(t.seen.front().bearerToken == "AT-build");

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("buildClient wires onTokensRefreshed -> Session persist") {
    auto path = freshTempPath();
    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    REQUIRE(Session::instance().setTokens(tokens("AT-old", "RT-old", 0)));

    // 401 -> refresh (200 with new triple) -> replay (200). The Session-
    // attached callback should then persist + reload.
    FakeTransport t;
    t.scripted.push_back(resp(kStatusUnauthorized));
    t.scripted.push_back(
        resp(kStatusOk, R"({"access_token":"AT-new","refresh_token":"RT-new",)"
                        R"("expires_at":2000})"));
    t.scripted.push_back(resp(kStatusOk, "{}"));

    auto client = Session::instance().buildClient(t.fn());
    REQUIRE(client.has_value());

    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = std::string(kBaseUrl) + "/v1/discover";
    auto r = client->send(req);
    CHECK(r.status == 200);

    auto inMem = Session::instance().currentTokens();
    REQUIRE(inMem.has_value());
    CHECK(inMem->accessToken == "AT-new");
    CHECK(inMem->refreshToken == "RT-new");
    CHECK(inMem->expiresAt == 2000);

    // Persistence: a fresh initialize() from the same path should re-load
    // the refreshed credential, proving the callback wrote disk.
    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    auto reloaded = Session::instance().currentTokens();
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->accessToken == "AT-new");

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("addAuthListener fires on setTokens and observes the new state") {
    auto path = freshTempPath();
    resetSession(path);

    int fired = 0;
    bool authedWhenFired = false;
    ScopedAuthListener listener([&] {
        ++fired;
        // Fired outside Session's lock and after state advances: a listener
        // can safely re-enter and see the post-change state.
        authedWhenFired = Session::instance().isAuthenticated();
    });

    REQUIRE(Session::instance().setTokens(tokens("AT", "RT", 1)));
    CHECK(fired == 1);
    CHECK(authedWhenFired);

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("clearTokens fires auth listener with logged-out state") {
    auto path = freshTempPath();
    resetSession(path);
    REQUIRE(Session::instance().setTokens(tokens("AT", "RT", 1)));

    int fired = 0;
    bool authedWhenFired = true;
    ScopedAuthListener listener([&] {
        ++fired;
        authedWhenFired = Session::instance().isAuthenticated();
    });

    Session::instance().clearTokens();
    CHECK(fired == 1);
    CHECK_FALSE(authedWhenFired);

    std::filesystem::remove(path);
}

TEST_CASE("removeAuthListener stops further notifications") {
    auto path = freshTempPath();
    resetSession(path);

    int fired = 0;
    auto id = Session::instance().addAuthListener([&] { ++fired; });
    Session::instance().removeAuthListener(id);

    REQUIRE(Session::instance().setTokens(tokens("AT", "RT", 1)));
    CHECK(fired == 0);

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("multiple auth listeners all fire") {
    auto path = freshTempPath();
    resetSession(path);

    int a = 0;
    int b = 0;
    ScopedAuthListener listenerA([&] { ++a; });
    ScopedAuthListener listenerB([&] { ++b; });

    REQUIRE(Session::instance().setTokens(tokens("AT", "RT", 1)));
    CHECK(a == 1);
    CHECK(b == 1);

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE("401 token refresh does not fire the auth listener") {
    auto path = freshTempPath();
    resetSession(path);
    REQUIRE(Session::instance().setTokens(tokens("AT-old", "RT-old", 0)));

    // Subscribe only after the login setTokens above, so any post-refresh
    // count is attributable to the refresh path alone.
    int fired = 0;
    ScopedAuthListener listener([&] { ++fired; });

    FakeTransport t;
    t.scripted.push_back(resp(kStatusUnauthorized));
    t.scripted.push_back(
        resp(kStatusOk, R"({"access_token":"AT-new","refresh_token":"RT-new",)"
                        R"("expires_at":2000})"));
    t.scripted.push_back(resp(kStatusOk, "{}"));

    auto client = Session::instance().buildClient(t.fn());
    REQUIRE(client.has_value());

    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = std::string(kBaseUrl) + "/v1/discover";
    auto r = client->send(req);
    CHECK(r.status == 200);

    // Refresh advanced the credential but kept the logged-in identity, so the
    // panel has nothing to re-evaluate -- the listener must stay silent.
    CHECK(fired == 0);
    CHECK(Session::instance().currentTokens()->accessToken == "AT-new");

    Session::instance().clearTokens();
    std::filesystem::remove(path);
}

TEST_CASE(
    "clearTokens invalidates pre-clear clients so their refresh callback "
    "cannot resurrect the credential") {
    auto path = freshTempPath();
    Session::instance().initialize(path, kBaseUrl, sampleDevice());
    REQUIRE(Session::instance().setTokens(tokens("AT-old", "RT-old", 0)));

    FakeTransport t;
    t.scripted.push_back(resp(kStatusUnauthorized));
    t.scripted.push_back(resp(
        kStatusOk, R"({"access_token":"AT-ghost","refresh_token":"RT-ghost",)"
                   R"("expires_at":9000})"));
    t.scripted.push_back(resp(kStatusOk, "{}"));

    auto client = Session::instance().buildClient(t.fn());
    REQUIRE(client.has_value());

    // User logs out *before* the in-flight refresh completes: the client we
    // still hold belongs to the previous generation.
    Session::instance().clearTokens();
    REQUIRE_FALSE(Session::instance().isAuthenticated());

    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = std::string(kBaseUrl) + "/v1/discover";
    (void)client->send(req);

    // The callback fired but found a stale generation -- Session must still
    // be logged out, and the file must stay absent.
    CHECK_FALSE(Session::instance().isAuthenticated());
    CHECK_FALSE(Session::instance().currentTokens().has_value());
    CHECK_FALSE(std::filesystem::exists(path));
}
