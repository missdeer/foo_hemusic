#include "api/platforms.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::parsePlatformInfo;
using hemusic::parsePlatformList;
using hemusic::PlatformInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parsePlatformInfo reads all fields") {
    auto p = parsePlatformInfo(parse(R"({
        "id": "  netease  ",
        "name": "  NetEase  ",
        "shortname": " NE ",
        "status": 1,
        "feature_support_flag": 13
    })"));

    CHECK(p.id == "netease");    // trimmed
    CHECK(p.name == "NetEase");  // trimmed
    CHECK(p.shortName == "NE");
    CHECK(p.status == 1);
    CHECK(p.available());
    CHECK(p.featureSupportFlag == 13ULL);
    // 13 = bits 0,2,3 set; supports() masks the bitfield.
    CHECK(p.supports(1ULL << 0));
    CHECK(p.supports(1ULL << 2));
    CHECK_FALSE(p.supports(1ULL << 1));
}

TEST_CASE("shortName falls back to name when shortname empty or absent") {
    SECTION("absent") {
        auto p = parsePlatformInfo(parse(R"({"id":"qq","name":"QQ Music"})"));
        CHECK(p.shortName == "QQ Music");
    }
    SECTION("present but empty/whitespace") {
        auto p = parsePlatformInfo(
            parse(R"({"id":"qq","name":"QQ Music","shortname":"  "})"));
        CHECK(p.shortName == "QQ Music");
    }
}

TEST_CASE("status and feature flag tolerate string-encoded numbers") {
    auto p = parsePlatformInfo(parse(R"({
        "id":"x","name":"X","status":"1","feature_support_flag":"6"
    })"));
    CHECK(p.status == 1);
    CHECK(p.available());
    CHECK(p.featureSupportFlag == 6ULL);
}

TEST_CASE("feature flag clamps junk and negatives to zero") {
    SECTION("non-numeric string") {
        auto p = parsePlatformInfo(
            parse(R"({"id":"x","name":"X","feature_support_flag":"abc"})"));
        CHECK(p.featureSupportFlag == 0ULL);
    }
    SECTION("negative number") {
        auto p = parsePlatformInfo(
            parse(R"({"id":"x","name":"X","feature_support_flag":-1})"));
        CHECK(p.featureSupportFlag == 0ULL);
    }
    SECTION(
        "a double yields 0 (Flutter parity, no UB from out-of-range cast)") {
        auto p = parsePlatformInfo(
            parse(R"({"id":"x","name":"X","feature_support_flag":1e100})"));
        CHECK(p.featureSupportFlag == 0ULL);
    }
}

TEST_CASE("feature flag preserves a high bit within uint64") {
    // 1<<47 is the highest defined capability bit; it must survive intact
    // (would be lost if parsed through a narrower or signed-truncating path).
    auto p = parsePlatformInfo(parse(
        R"({"id":"x","name":"X","feature_support_flag":140737488355328})"));
    CHECK(p.featureSupportFlag == (1ULL << 47));
    CHECK(p.supports(1ULL << 47));
}

TEST_CASE("status defaults to unavailable when absent") {
    auto p = parsePlatformInfo(parse(R"({"id":"x","name":"X"})"));
    CHECK(p.status == 0);
    CHECK_FALSE(p.available());
}

TEST_CASE("parsePlatformList reads the list envelope and drops invalid items") {
    auto list = parsePlatformList(parse(R"({
        "list": [
            {"id":"netease","name":"NetEase","status":1},
            {"id":"","name":"NoId"},
            {"id":"noname","name":""},
            "not-an-object",
            {"id":"qq","name":"QQ Music","status":0}
        ]
    })"));

    REQUIRE(list.size() == 2);  // empty id, empty name, and scalar all dropped
    CHECK(list.at(0).id == "netease");
    CHECK(list.at(0).available());
    CHECK(list.at(1).id == "qq");
    CHECK_FALSE(list.at(1).available());
}

TEST_CASE(
    "parsePlatformList degrades to empty on a missing or non-array list") {
    CHECK(parsePlatformList(parse(R"({})")).empty());
    CHECK(parsePlatformList(parse(R"({"list":"oops"})")).empty());
    CHECK(parsePlatformList(parse(R"([])")).empty());  // non-object body
}
