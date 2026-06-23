#include "api/artist_detail.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::ArtistDetailContent;
using hemusic::parseArtistAlbumsPage;
using hemusic::parseArtistDetailContent;
using hemusic::parseArtistMvsPage;
using hemusic::parseArtistSongsPage;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parseArtistDetailContent reads meta + embedded songs") {
    auto c = parseArtistDetailContent(parse(R"({
        "name": "Jay Chou",
        "pic": "http://x/a.jpg",
        "description": "Mando-pop",
        "mv_count": 12,
        "song_count": 340,
        "album_count": 15,
        "alias": "JC",
        "songs": [{"id":"s1","name":"Track"}]
    })"),
                                      "ar1", "netease", "Fallback");

    CHECK(c.info.id == "ar1");            // from request
    CHECK(c.info.platform == "netease");  // from request
    CHECK(c.info.name == "Jay Chou");
    CHECK(c.info.cover == "http://x/a.jpg");
    CHECK(c.info.description == "Mando-pop");
    CHECK(c.info.mvCount == "12");
    CHECK(c.info.songCount == "340");
    CHECK(c.info.albumCount == "15");
    CHECK(c.info.alias == "JC");
    REQUIRE(c.songs.size() == 1);
    CHECK(c.songs.at(0).platform == "netease");  // fallback propagates
}

TEST_CASE("artist detail resolves songs from alternate / nested keys") {
    SECTION("alternate top-level key 'tracks'") {
        auto c = parseArtistDetailContent(
            parse(R"({"tracks":[{"id":"s1","name":"A"}]})"), "ar1", "qq", "T");
        REQUIRE(c.songs.size() == 1);
        CHECK(c.songs.at(0).name == "A");
    }
    SECTION("nested under artist.song_list (artist parent is detail-only)") {
        auto c = parseArtistDetailContent(
            parse(R"({"artist":{"song_list":[{"id":"s1","name":"A"}]}})"),
            "ar1", "qq", "T");
        REQUIRE(c.songs.size() == 1);
        CHECK(c.songs.at(0).name == "A");
    }
    SECTION("no song list anywhere -> empty") {
        auto c = parseArtistDetailContent(parse(R"({"name":"X"})"), "ar1", "qq",
                                          "T");
        CHECK(c.songs.empty());
    }
}

TEST_CASE("artist detail applies detail-specific fallbacks") {
    SECTION("empty body name falls back to request title") {
        auto c = parseArtistDetailContent(parse(R"({})"), "ar1", "qq", "Title");
        CHECK(c.info.name == "Title");
    }
    SECTION("counts clamp negative values to 0 (display safety)") {
        auto c = parseArtistDetailContent(
            parse(R"({"song_count":-1,"album_count":-2,"mv_count":-3})"), "ar1",
            "qq", "T");
        CHECK(c.info.songCount == "0");
        CHECK(c.info.albumCount == "0");
        CHECK(c.info.mvCount == "0");
    }
    SECTION("mv_count null falls through to video_count") {
        auto c = parseArtistDetailContent(
            parse(R"({"mv_count":null,"video_count":9})"), "ar1", "qq", "T");
        CHECK(c.info.mvCount == "9");
    }
}

TEST_CASE("parseArtistSongsPage reads { list, has_more }") {
    auto chunk = parseArtistSongsPage(
        parse(R"({"list":[{"id":"s1","name":"A"},{"id":"s2","name":"B"}],
                  "has_more":true})"),
        "qq");
    REQUIRE(chunk.items.size() == 2);
    CHECK(chunk.items.at(0).platform == "qq");  // fallback propagates
    CHECK(chunk.hasMore);
}

TEST_CASE("parseArtistSongsPage drops player-unsafe entries") {
    // Mirrors the api/ layer's player-safety stance (playlist_detail.h /
    // album_detail.h): an entry without id+name is unplayable so the lenient
    // parser drops it.
    auto chunk = parseArtistSongsPage(parse(R"({"list":[{"id":"s1","name":"Ok"},
                          {"id":"","name":"NoId"},
                          {"id":"s3","name":""}]})"),
                                      "qq");
    REQUIRE(chunk.items.size() == 1);
    CHECK(chunk.items.at(0).id == "s1");
    CHECK_FALSE(chunk.hasMore);  // absent has_more -> false
}

TEST_CASE("parseArtistAlbumsPage drops player-unsafe entries") {
    auto chunk =
        parseArtistAlbumsPage(parse(R"({"list":[{"id":"al1","name":"Fantasy",
                           "artists":[{"id":"ar1","name":"Jay"}]},
                          {"id":"","name":"x"},
                          {"id":"al2","name":""}],
                  "has_more":false})"),
                              "qq");
    REQUIRE(chunk.items.size() == 1);
    CHECK(chunk.items.at(0).id == "al1");
    CHECK(chunk.items.at(0).platform == "qq");
    REQUIRE(chunk.items.at(0).artists.size() == 1);
    CHECK(chunk.items.at(0).artists.at(0).name == "Jay");
}

TEST_CASE("parseArtistMvsPage drops player-unsafe entries + reads has_more") {
    auto chunk = parseArtistMvsPage(
        parse(R"({"list":[{"id":"mv1","name":"Clip","creator":"Jay"},
                          {"id":"","name":"Bad"}],
                  "has_more":"true"})"),
        "qq");
    REQUIRE(chunk.items.size() == 1);
    CHECK(chunk.items.at(0).id == "mv1");
    CHECK(chunk.items.at(0).platform == "qq");
    CHECK(chunk.items.at(0).creator == "Jay");
    CHECK(chunk.hasMore);  // "true" string parses as bool
}

TEST_CASE("parseArtistAlbumsPage widens count/publishTime aliases") {
    // Artist endpoint adds trackCount / createTime aliases beyond the shared
    // AlbumInfo parser (matches Flutter artist client). Used only when the
    // shared parser returned an empty/zero default.
    SECTION("trackCount fills in when song_count/songCount absent") {
        auto chunk = parseArtistAlbumsPage(
            parse(R"({"list":[{"id":"al1","name":"X","trackCount":7}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).songCount == "7");
    }
    SECTION("createTime fills in when publish_time absent") {
        auto chunk = parseArtistAlbumsPage(
            parse(R"({"list":[{"id":"al1","name":"X","createTime":"2024"}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).publishTime == "2024");
    }
    SECTION("explicit song_count wins over trackCount") {
        auto chunk =
            parseArtistAlbumsPage(parse(R"({"list":[{"id":"al1","name":"X",
                                "song_count":12,"trackCount":7}]})"),
                                  "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).songCount == "12");
    }
    SECTION("explicit song_count == 0 wins over trackCount (Codex R2)") {
        // The shared parser clamps song_count=0 to "0", so a naive "empty or
        // 0 -> fall back" check would clobber it with trackCount. Probe the
        // source for primary key presence instead.
        auto chunk = parseArtistAlbumsPage(
            parse(
                R"({"list":[{"id":"al1","name":"X","song_count":0,"trackCount":7}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).songCount == "0");
    }
    SECTION("explicit publish_time wins over createTime even when empty") {
        // Explicit empty publish_time is still authoritative -- the artist
        // endpoint must not silently substitute createTime when the server
        // signaled "no publish time" via an empty string.
        auto chunk = parseArtistAlbumsPage(
            parse(
                R"({"list":[{"id":"al1","name":"X","publish_time":"","createTime":"2024"}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).publishTime.empty());
    }
}

TEST_CASE("parseArtistMvsPage widens playCount with watch_count alias") {
    SECTION("watch_count used when play_count/playCount absent") {
        auto chunk = parseArtistMvsPage(
            parse(R"({"list":[{"id":"mv1","name":"X","watch_count":42}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).playCount == "42");
    }
    SECTION("explicit play_count wins over watch_count") {
        auto chunk = parseArtistMvsPage(
            parse(
                R"({"list":[{"id":"mv1","name":"X","play_count":9,"watch_count":42}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).playCount == "9");
    }
    SECTION("explicit play_count == 0 wins over watch_count (Codex R2)") {
        auto chunk = parseArtistMvsPage(
            parse(
                R"({"list":[{"id":"mv1","name":"X","play_count":0,"watch_count":42}]})"),
            "qq");
        REQUIRE(chunk.items.size() == 1);
        CHECK(chunk.items.at(0).playCount == "0");
    }
}

TEST_CASE("page chunks tolerate non-object / missing list") {
    SECTION("non-object body") {
        auto c = parseArtistSongsPage(parse("[]"));
        CHECK(c.items.empty());
        CHECK_FALSE(c.hasMore);
    }
    SECTION("missing list key") {
        auto c = parseArtistAlbumsPage(parse(R"({"has_more":true})"));
        CHECK(c.items.empty());
        CHECK(c.hasMore);
    }
    SECTION("list is non-array (defensive)") {
        auto c = parseArtistMvsPage(parse(R"({"list":"oops"})"));
        CHECK(c.items.empty());
    }
}
