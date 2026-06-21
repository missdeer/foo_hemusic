#include "api/album_detail.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::AlbumInfo;
using hemusic::parseAlbumDetailInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parseAlbumDetailInfo reads meta + embedded songs") {
    auto a = parseAlbumDetailInfo(parse(R"({
        "name": "Fantasy",
        "pic": "http://x/a.jpg",
        "artists": [{"id":"ar1","name":"Jay"}],
        "song_count": 12,
        "publish_time": "2001-09",
        "songs": [{"id":"s1","name":"Track"}],
        "description": "classic",
        "language": "zh",
        "genre": "pop",
        "type": 1,
        "is_finished": true,
        "play_count": "9999"
    })"),
                                  "al1", "netease", "Fallback");

    CHECK(a.name == "Fantasy");
    CHECK(a.id == "al1");            // from request
    CHECK(a.platform == "netease");  // from request
    CHECK(a.cover == "http://x/a.jpg");
    REQUIRE(a.artists.size() == 1);
    CHECK(a.artists.at(0).name == "Jay");
    CHECK(a.songCount == "12");
    CHECK(a.publishTime == "2001-09");
    REQUIRE(a.songs.size() == 1);
    CHECK(a.songs.at(0).platform == "netease");  // fallback propagates
    CHECK(a.description == "classic");
    CHECK(a.language == "zh");
    CHECK(a.genre == "pop");
    CHECK(a.type == 1);
    CHECK(a.isFinished);
    CHECK(a.playCount == "9999");
}

TEST_CASE("album detail resolves songs from alternate / nested keys") {
    SECTION("alternate top-level key 'tracks'") {
        auto a = parseAlbumDetailInfo(
            parse(
                R"({"tracks":[{"id":"s1","name":"A"},{"id":"s2","name":"B"}]})"),
            "al1", "qq", "T");
        CHECK(a.songs.size() == 2);
    }
    SECTION("nested under data.song_list") {
        auto a = parseAlbumDetailInfo(
            parse(R"({"data":{"song_list":[{"id":"s1","name":"A"}]}})"), "al1",
            "qq", "T");
        REQUIRE(a.songs.size() == 1);
        CHECK(a.songs.at(0).name == "A");
    }
    SECTION("no song list anywhere -> empty") {
        auto a =
            parseAlbumDetailInfo(parse(R"({"name":"X"})"), "al1", "qq", "T");
        CHECK(a.songs.empty());
    }
}

TEST_CASE("album detail applies detail-specific fallbacks") {
    SECTION("song_count falls back to the resolved song count when absent") {
        auto a = parseAlbumDetailInfo(
            parse(
                R"({"songs":[{"id":"s1","name":"A"},{"id":"s2","name":"B"}]})"),
            "al1", "qq", "T");
        CHECK(a.songCount == "2");  // len(songs), not "0"
    }
    SECTION("publish_time tolerates the createTime alias") {
        auto a = parseAlbumDetailInfo(parse(R"({"createTime":"2020"})"), "al1",
                                      "qq", "T");
        CHECK(a.publishTime == "2020");
    }
    SECTION("empty body name falls back to request title") {
        auto a = parseAlbumDetailInfo(parse(R"({})"), "al1", "qq", "My Album");
        CHECK(a.name == "My Album");
    }
    SECTION("artists ignore the 'artist' alias (detail reads only 'artists')") {
        // The shared parseAlbumInfo coalesces artists/artist; detail does not.
        auto a = parseAlbumDetailInfo(parse(R"({"artist":"Solo"})"), "al1",
                                      "qq", "T");
        CHECK(a.artists.empty());
    }
}
