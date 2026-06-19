#include "auth/oauth_flow.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include "auth/device_info.h"

using hemusic::AuthStatus;
using hemusic::buildRefreshRequest;
using hemusic::buildSessionRequest;
using hemusic::DeviceInfo;
using hemusic::makeDeviceInfo;
using hemusic::parseAuthCodeUrl;
using hemusic::parseAuthProviders;
using hemusic::parseAuthStatus;
using hemusic::parseAuthToken;

namespace {
DeviceInfo sampleDevice() {
    return makeDeviceInfo("flutter_windows_abc", "MY-PC", "0.0.1");
}
boost::json::value parse(std::string_view s) { return boost::json::parse(s); }
}  // namespace

// redirect_uri must be omitted (not sent empty) so the backend falls back to
// its configured default callback -- the whole point of the no-loopback design.
TEST_CASE("buildSessionRequest omits redirect_uri when empty", "[oauth]") {
    auto body = buildSessionRequest("linuxdo", "", sampleDevice());

    REQUIRE(body.at("provider").as_string() == "linuxdo");
    REQUIRE_FALSE(body.contains("redirect_uri"));
    REQUIRE(body.contains("device_info"));
    // device_info carries the Flutter masquerade contract, not an empty object.
    REQUIRE(body.at("device_info").as_object().at("app_type").as_string() ==
            "flutter");
}

TEST_CASE("buildSessionRequest includes redirect_uri when provided",
          "[oauth]") {
    auto body = buildSessionRequest("linuxdo", "hemusic://cb", sampleDevice());
    REQUIRE(body.at("redirect_uri").as_string() == "hemusic://cb");
}

TEST_CASE("buildRefreshRequest carries refresh_token + device_info",
          "[oauth]") {
    auto body = buildRefreshRequest("refresh-xyz", sampleDevice());
    REQUIRE(body.at("refresh_token").as_string() == "refresh-xyz");
    REQUIRE(body.contains("device_info"));
}

TEST_CASE("parseAuthProviders extracts and trims the list", "[oauth]") {
    auto list =
        parseAuthProviders(parse(R"({"list":["linuxdo"," "," github "]})"));
    // Whitespace-only entries drop out; real ids are trimmed.
    REQUIRE(list == std::vector<std::string>{"linuxdo", "github"});
}

TEST_CASE("parseAuthProviders tolerates a missing/!array list", "[oauth]") {
    REQUIRE(parseAuthProviders(parse(R"({})")).empty());
    REQUIRE(parseAuthProviders(parse(R"({"list":"nope"})")).empty());
}

// check_interval drives the poll loop; the backend sometimes ships ints as
// strings, so the int coercion (Flutter _asInt) is contract, not convenience.
TEST_CASE("parseAuthCodeUrl coerces numeric strings", "[oauth]") {
    auto r = parseAuthCodeUrl(parse(
        R"({"url":" http://x ","state":"st","check_interval":"5","expires_at":123})"));
    REQUIRE(r.url == "http://x");  // trimmed
    REQUIRE(r.state == "st");
    REQUIRE(r.checkInterval == 5);
    REQUIRE(r.expiresAt == 123);
}

TEST_CASE("parseAuthStatus classifies the status string", "[oauth]") {
    REQUIRE(parseAuthStatus(parse(R"({"status":"pending"})")).kind() ==
            AuthStatus::Pending);
    REQUIRE(parseAuthStatus(parse(R"({"status":"success"})")).kind() ==
            AuthStatus::Success);
    REQUIRE(parseAuthStatus(parse(R"({"status":"expired"})")).kind() ==
            AuthStatus::Expired);

    // "failed" is the OAuth poll's terminal failure status (Flutter
    // login_page _isOAuthFailedStatus), with the reason in `error`.
    auto failed =
        parseAuthStatus(parse(R"({"status":"failed","error":"denied"})"));
    REQUIRE(failed.kind() == AuthStatus::Failed);
    REQUIRE(failed.error == "denied");

    auto err = parseAuthStatus(parse(R"({"status":"error","error":"boom"})"));
    REQUIRE(err.kind() == AuthStatus::Error);
    REQUIRE(err.error == "boom");

    // Anything unrecognized must not masquerade as success/pending.
    REQUIRE(parseAuthStatus(parse(R"({"status":"weird"})")).kind() ==
            AuthStatus::Unknown);
}

TEST_CASE("parseAuthToken reads the new triple", "[oauth]") {
    auto t = parseAuthToken(parse(
        R"({"access_token":"acc","refresh_token":"ref","expires_at":99})"));
    REQUIRE(t.accessToken == "acc");
    REQUIRE(t.refreshToken == "ref");
    REQUIRE(t.expiresAt == 99);
}

// Legacy backends return a single {token} (or {data:{token}}); the fallback
// keeps those logins working (api.md sec.0.6). Without it old servers break.
TEST_CASE("parseAuthToken falls back to legacy single token", "[oauth]") {
    auto top = parseAuthToken(parse(R"({"token":"legacy"})"));
    REQUIRE(top.accessToken == "legacy");
    REQUIRE(top.refreshToken.empty());

    auto nested = parseAuthToken(parse(R"({"data":{"token":"legacy2"}})"));
    REQUIRE(nested.accessToken == "legacy2");
}

// A present access_token always wins over a stray legacy token field.
TEST_CASE("parseAuthToken prefers access_token over legacy token", "[oauth]") {
    auto t =
        parseAuthToken(parse(R"({"access_token":"acc","token":"legacy"})"));
    REQUIRE(t.accessToken == "acc");
}
