#include "api/user.h"

#include <string_view>

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::parseUserInfo;
using hemusic::UserInfo;

namespace {
boost::json::value parse(std::string_view s) { return boost::json::parse(s); }
}  // namespace

TEST_CASE("parseUserInfo maps the full profile", "[user]") {
    auto info = parseUserInfo(parse(R"({
        "id": "42",
        "username": "alice",
        "nickname": "Alice L.",
        "email": "a@example.com",
        "status": 1,
        "avatar": "https://cdn/x.png"
    })"));

    REQUIRE(info.valid());
    REQUIRE(info.id == "42");
    REQUIRE(info.username == "alice");
    REQUIRE(info.nickname == "Alice L.");
    REQUIRE(info.email == "a@example.com");
    REQUIRE(info.status == 1);
    REQUIRE(info.avatarUrl == "https://cdn/x.png");
}

// Backend is loose with scalar types; status may arrive as a string and id as a
// number -- Flutter's _asInt / _asString swallow both, so must we.
TEST_CASE("parseUserInfo coerces string status and numeric id", "[user]") {
    auto info =
        parseUserInfo(parse(R"({"id": 7, "username": "bob", "status": "3"})"));

    REQUIRE(info.id == "7");
    REQUIRE(info.username == "bob");
    REQUIRE(info.status == 3);
    REQUIRE(info.valid());
}

TEST_CASE("parseUserInfo defaults missing fields and trims strings", "[user]") {
    auto info = parseUserInfo(parse(R"({"id": "  9  ", "email": null})"));

    REQUIRE(info.id == "9");  // trimmed
    REQUIRE(info.username.empty());
    REQUIRE(info.email.empty());  // null -> ""
    REQUIRE(info.status == 0);
}

// Token verification gates on valid(): no id means no usable identity, even if
// the call returned 200 with junk.
TEST_CASE("parseUserInfo is invalid without an id", "[user]") {
    REQUIRE_FALSE(parseUserInfo(parse(R"({"username": "x"})")).valid());
    REQUIRE_FALSE(parseUserInfo(parse("[]")).valid());        // non-object
    REQUIRE_FALSE(parseUserInfo(parse("\"oops\"")).valid());  // scalar
}
