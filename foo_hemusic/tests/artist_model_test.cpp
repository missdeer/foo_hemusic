#include "api/artist.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::ArtistInfo;
using hemusic::parseArtistInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parseArtistInfo reads all fields") {
    auto a = parseArtistInfo(parse(R"({
        "id": "ar1",
        "name": "Jay Chou",
        "pic": "http://x/a.jpg",
        "platform": "netease",
        "description": "Mando-pop",
        "mv_count": 12,
        "song_count": 340,
        "album_count": 15,
        "alias": "JC"
    })"),
                             "qq");

    CHECK(a.id == "ar1");
    CHECK(a.name == "Jay Chou");
    CHECK(a.cover == "http://x/a.jpg");  // via the 'pic' alias
    CHECK(a.platform == "netease");      // explicit platform wins over fallback
    CHECK(a.description == "Mando-pop");
    CHECK(a.mvCount == "12");
    CHECK(a.songCount == "340");
    CHECK(a.albumCount == "15");
    CHECK(a.alias == "JC");
}

TEST_CASE("mvCount follows the mv_count ?? mvCount ?? video_count chain") {
    SECTION("mv_count takes priority") {
        auto a = parseArtistInfo(
            parse(R"({"mv_count":3,"mvCount":7,"video_count":9})"));
        CHECK(a.mvCount == "3");
    }
    SECTION("mvCount used when mv_count absent") {
        auto a = parseArtistInfo(parse(R"({"mvCount":7,"video_count":9})"));
        CHECK(a.mvCount == "7");
    }
    SECTION("video_count used when both prior keys absent") {
        auto a = parseArtistInfo(parse(R"({"video_count":9})"));
        CHECK(a.mvCount == "9");
    }
    SECTION("a present-but-null prior key falls through to the next") {
        // `null ?? null ?? 9` must reach video_count, not stop at the null.
        auto a = parseArtistInfo(
            parse(R"({"mv_count":null,"mvCount":null,"video_count":9})"));
        CHECK(a.mvCount == "9");
    }
    SECTION("all absent -> 0") {
        auto a = parseArtistInfo(parse(R"({})"));
        CHECK(a.mvCount == "0");
    }
}

TEST_CASE("count fields tolerate camelCase aliases and clamp to 0") {
    SECTION("camelCase aliases") {
        auto a = parseArtistInfo(parse(R"({"songCount":7,"albumCount":2})"));
        CHECK(a.songCount == "7");
        CHECK(a.albumCount == "2");
    }
    SECTION("negative / junk counts render as 0, never a negative string") {
        // A count is a display string; the UI must never show "-1".
        auto a = parseArtistInfo(
            parse(R"({"song_count":-5,"album_count":"abc","mv_count":-1})"));
        CHECK(a.songCount == "0");
        CHECK(a.albumCount == "0");
        CHECK(a.mvCount == "0");
    }
}

TEST_CASE("cover alias priority cover > pic > imgurl > image > thumb") {
    auto a = parseArtistInfo(
        parse(R"({"thumb":"t","image":"i","imgurl":"g","pic":"p"})"));
    CHECK(a.cover == "p");  // first non-empty in priority order
}

TEST_CASE("platform falls back only when absent") {
    SECTION("absent platform uses fallback") {
        auto a = parseArtistInfo(parse(R"({"id":"ar1","name":"X"})"), "qq");
        CHECK(a.platform == "qq");
    }
    SECTION("present platform overrides fallback") {
        auto a = parseArtistInfo(
            parse(R"({"id":"ar1","name":"X","platform":"kugou"})"), "qq");
        CHECK(a.platform == "kugou");
    }
}

TEST_CASE("non-object yields an empty ArtistInfo") {
    auto a = parseArtistInfo(parse("[]"), "qq");
    CHECK(a.id.empty());
    CHECK(a.name.empty());
    CHECK(a.mvCount == "0");
    CHECK(a.songCount == "0");
    CHECK(a.albumCount == "0");
    // fallbackPlatform is not applied to a non-object (matches the all-empty
    // object path: platform stays the fallback only when the object lacks it).
    CHECK(a.platform == "qq");
}
