#include "api/radio.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include "api/platforms.h"

using hemusic::kFeatureListRadios;
using hemusic::parseRadioGroupInfo;
using hemusic::parseRadioGroups;
using hemusic::parseRadioInfo;
using hemusic::parseRadioSongs;
using hemusic::PlatformInfo;
using hemusic::RadioGroupInfo;
using hemusic::RadioInfo;
using hemusic::resolveRadioPlatform;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parseRadioInfo reads fields and falls back platform") {
    SECTION("all fields, cover via alias, explicit platform wins") {
        auto r = parseRadioInfo(
            parse(R"({"name":"Jazz","id":"r1","pic":"http://x/c.jpg",
                      "platform":"netease"})"),
            "qq");
        CHECK(r.name == "Jazz");
        CHECK(r.id == "r1");
        CHECK(r.cover == "http://x/c.jpg");
        CHECK(r.platform == "netease");
    }
    SECTION("absent platform uses fallback") {
        auto r = parseRadioInfo(parse(R"({"name":"Jazz","id":"r1"})"), "qq");
        CHECK(r.platform == "qq");
    }
}

TEST_CASE("parseRadioGroupInfo nests radios under the group's platform") {
    auto g = parseRadioGroupInfo(parse(R"({
        "name": "Moods",
        "platform": "netease",
        "radios": [
            {"name":"Chill","id":"r1"},
            {"name":"","id":"r2"},
            {"name":"Focus","id":""},
            {"name":"Energy","id":"r4","platform":"kugou"}
        ]
    })"),
                                 "qq");

    CHECK(g.name == "Moods");
    CHECK(g.platform == "netease");
    // empty-name and empty-id radios are dropped (Flutter `_radios` filter).
    REQUIRE(g.radios.size() == 2);
    CHECK(g.radios.at(0).name == "Chill");
    // child inherits the group's resolved platform as fallback.
    CHECK(g.radios.at(0).platform == "netease");
    // an explicit child platform still overrides the inherited fallback.
    CHECK(g.radios.at(1).name == "Energy");
    CHECK(g.radios.at(1).platform == "kugou");
}

TEST_CASE(
    "parseRadioGroupInfo resolves group platform from fallback for kids") {
    // group has no platform of its own -> resolves to fallback, and that
    // resolved value is what the children inherit.
    auto g = parseRadioGroupInfo(
        parse(R"({"name":"G","radios":[{"name":"A","id":"a"}]})"), "qq");
    CHECK(g.platform == "qq");
    REQUIRE(g.radios.size() == 1);
    CHECK(g.radios.at(0).platform == "qq");
}

TEST_CASE("parseRadioGroups reads the groups envelope, degrades on bad input") {
    SECTION("well-formed groups list") {
        auto groups = parseRadioGroups(parse(R"({
            "groups": [
                {"name":"G1","radios":[{"name":"A","id":"a"}]},
                "junk",
                {"name":"G2","radios":[]}
            ]
        })"),
                                       "qq");
        REQUIRE(groups.size() ==
                2);  // non-object 'junk' skipped, not an empty shell
        CHECK(groups.at(0).name == "G1");
        CHECK(groups.at(0).radios.size() == 1);
        CHECK(groups.at(1).radios.empty());
    }
    SECTION("missing groups key -> empty (lenient, not a throw)") {
        CHECK(parseRadioGroups(parse(R"({})"), "qq").empty());
    }
    SECTION("non-array groups -> empty") {
        CHECK(parseRadioGroups(parse(R"({"groups":"oops"})"), "qq").empty());
    }
    SECTION("non-object body -> empty") {
        CHECK(parseRadioGroups(parse("[]"), "qq").empty());
    }
}

TEST_CASE("parseRadioSongs reads list and drops unplayable songs") {
    SECTION("valid + unplayable mix, fallback platform propagates") {
        auto songs = parseRadioSongs(parse(R"({
            "list": [
                {"id":"s1","name":"Track"},
                {"id":"","name":"NoId"},
                {"id":"s3","name":""}
            ]
        })"),
                                     "qq");
        REQUIRE(songs.size() == 1);  // empty id/name dropped
        CHECK(songs.at(0).name == "Track");
        CHECK(songs.at(0).platform == "qq");
    }
    SECTION("missing list -> empty") {
        CHECK(parseRadioSongs(parse(R"({})"), "qq").empty());
    }
}

TEST_CASE("resolveRadioPlatform picks first available radio-capable platform") {
    PlatformInfo unsupported;
    unsupported.id = "x";
    unsupported.name = "X";
    unsupported.status = 1;  // available but no radio bit
    unsupported.featureSupportFlag = 0;

    PlatformInfo unavailable;
    unavailable.id = "y";
    unavailable.name = "Y";
    unavailable.status = 0;
    unavailable.featureSupportFlag = kFeatureListRadios;

    PlatformInfo good;
    good.id = "z";
    good.name = "Z";
    good.status = 1;
    good.featureSupportFlag = kFeatureListRadios;

    SECTION("empty list -> nullopt") {
        CHECK_FALSE(resolveRadioPlatform({}).has_value());
    }
    SECTION("no candidate has the radio bit -> nullopt") {
        CHECK_FALSE(resolveRadioPlatform({unsupported}).has_value());
    }
    SECTION("candidate has the bit but is unavailable -> nullopt") {
        CHECK_FALSE(resolveRadioPlatform({unavailable}).has_value());
    }
    SECTION("picks first qualifying entry, ignoring earlier disqualified") {
        auto p = resolveRadioPlatform({unsupported, unavailable, good});
        REQUIRE(p.has_value());
        CHECK(p->id == "z");
    }
}
