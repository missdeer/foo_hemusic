#include "api/playlist_detail.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::parsePlaylistDetailInfo;
using hemusic::parsePlaylistSongs;
using hemusic::PlaylistInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parsePlaylistDetailInfo builds meta from body + request") {
    auto p = parsePlaylistDetailInfo(parse(R"({
        "name": "Chill Mix",
        "pic": "http://x/c.jpg",
        "creator": "DJ",
        "song_count": 42,
        "play_count": 12345,
        "description": "easy listening"
    })"),
                                     "pl1", "netease", "Fallback Title");

    CHECK(p.name == "Chill Mix");
    CHECK(p.id == "pl1");                // from request, not body
    CHECK(p.platform == "netease");      // from request
    CHECK(p.cover == "http://x/c.jpg");  // via 'pic' alias
    CHECK(p.creator == "DJ");
    CHECK(p.songCount == "42");
    CHECK(p.playCount == "12345");
    CHECK(p.description == "easy listening");
    CHECK(p.songs.empty());  // filled separately from /v1/playlist/songs
}

TEST_CASE("playlist detail meta applies request/Flutter-specific fallbacks") {
    SECTION("empty body name falls back to the request title") {
        auto p =
            parsePlaylistDetailInfo(parse(R"({})"), "pl1", "qq", "My List");
        CHECK(p.name == "My List");
    }
    SECTION("empty creator becomes '-'") {
        auto p =
            parsePlaylistDetailInfo(parse(R"({"name":"X"})"), "pl1", "qq", "T");
        CHECK(p.creator == "-");
    }
    SECTION("song_count tolerates the trackCount alias") {
        auto p = parsePlaylistDetailInfo(parse(R"({"trackCount":"7"})"), "pl1",
                                         "qq", "T");
        CHECK(p.songCount == "7");
    }
    SECTION("counts stay empty (NOT '0') when absent -- detail-specific rule") {
        // Unlike the shared parsePlaylistInfo which clamps to "0", the detail
        // meta keeps the raw string so the UI can hide an unknown count.
        auto p =
            parsePlaylistDetailInfo(parse(R"({"name":"X"})"), "pl1", "qq", "T");
        CHECK(p.songCount.empty());
        CHECK(p.playCount.empty());
    }
}

TEST_CASE("parsePlaylistSongs reads list and drops unplayable songs") {
    SECTION("valid + unplayable mix, fallback platform propagates") {
        auto songs = parsePlaylistSongs(parse(R"({
            "list": [
                {"id":"s1","name":"Track"},
                {"id":"","name":"NoId"},
                {"id":"s3","name":""}
            ]
        })"),
                                        "qq");
        REQUIRE(songs.size() == 1);
        CHECK(songs.at(0).name == "Track");
        CHECK(songs.at(0).platform == "qq");
    }
    SECTION("missing list -> empty") {
        CHECK(parsePlaylistSongs(parse(R"({})"), "qq").empty());
    }
    SECTION("non-array list -> empty") {
        CHECK(parsePlaylistSongs(parse(R"({"list":"oops"})"), "qq").empty());
    }
}
