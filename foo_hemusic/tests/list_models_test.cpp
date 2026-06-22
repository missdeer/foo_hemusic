#include "api/album.h"
#include "api/playlist.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::AlbumInfo;
using hemusic::artistNamesText;
using hemusic::parseAlbumInfo;
using hemusic::parsePlaylistInfo;
using hemusic::PlaylistInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parsePlaylistInfo reads all fields including songs and categories") {
    auto p = parsePlaylistInfo(parse(R"({
        "name": "Chill Mix",
        "id": "pl1",
        "pic": "http://x/c.jpg",
        "creator": "DJ",
        "song_count": 42,
        "play_count": 12345,
        "songs": [{"id":"s1","name":"Track"}],
        "platform": "netease",
        "description": "easy listening",
        "categories": [{"name":"Pop","id":"c1"}, "Rock", {"name":"","id":"x"}],
        "is_default": 1
    })"),
                               "netease");

    CHECK(p.name == "Chill Mix");
    CHECK(p.id == "pl1");
    CHECK(p.cover == "http://x/c.jpg");  // via the 'pic' alias
    CHECK(p.creator == "DJ");
    CHECK(p.songCount == "42");
    CHECK(p.playCount == "12345");
    REQUIRE(p.songs.size() == 1);
    CHECK(p.songs.at(0).name == "Track");
    CHECK(p.songs.at(0).platform ==
          "netease");  // fallback propagates into songs
    CHECK(p.platform == "netease");
    CHECK(p.description == "easy listening");
    REQUIRE(p.categories.size() == 2);  // empty-name category dropped
    CHECK(p.categories.at(0).name == "Pop");
    CHECK(p.categories.at(1).name == "Rock");  // bare string -> name-only
    CHECK(p.isDefault);
}

TEST_CASE("playlist count fields tolerate camelCase aliases and clamp to 0") {
    SECTION("camelCase aliases") {
        auto p = parsePlaylistInfo(parse(R"({"songCount":7,"playCount":99})"));
        CHECK(p.songCount == "7");
        CHECK(p.playCount == "99");
    }
    SECTION("negative or absent counts render as 0, never a negative string") {
        // A count is a display string; the UI must never show "-1", so the rule
        // clamps junk/negatives to "0".
        auto p = parsePlaylistInfo(parse(R"({"song_count":-5})"));
        CHECK(p.songCount == "0");
        CHECK(p.playCount == "0");  // absent
    }
}

TEST_CASE("playlist isDefault is true on the int 1 or any truthy bool/string") {
    // Flutter's rule is `_int(v) == 1 || _bool(v)`, so several encodings of
    // "default" must all resolve true -- the backend is inconsistent here.
    CHECK(parsePlaylistInfo(parse(R"({"is_default":1})")).isDefault);
    CHECK(parsePlaylistInfo(parse(R"({"is_default":true})")).isDefault);
    CHECK(parsePlaylistInfo(parse(R"({"is_default":"true"})")).isDefault);
    CHECK(parsePlaylistInfo(parse(R"({"is_default":"1"})")).isDefault);

    CHECK_FALSE(parsePlaylistInfo(parse(R"({"is_default":0})")).isDefault);
    CHECK_FALSE(
        parsePlaylistInfo(parse(R"({"is_default":"false"})")).isDefault);
    CHECK_FALSE(parsePlaylistInfo(parse(R"({})")).isDefault);
}

TEST_CASE("playlist platform falls back when absent or empty") {
    CHECK(parsePlaylistInfo(parse(R"({"id":"1","name":"n"})"), "qq").platform ==
          "qq");
    CHECK(parsePlaylistInfo(parse(R"({"platform":""})"), "qq").platform ==
          "qq");
    CHECK(parsePlaylistInfo(parse(R"({"platform":"kw"})"), "qq").platform ==
          "kw");
}

TEST_CASE("parseAlbumInfo reads all fields and tolerates the artist alias") {
    auto a = parseAlbumInfo(parse(R"({
        "name": "Album X",
        "id": "al1",
        "image": "http://x/a.jpg",
        "artist": "Solo Artist",
        "song_count": 10,
        "publish_time": "2021-05-01",
        "songs": [{"id":"s1","name":"Track"}],
        "description": "a record",
        "platform": "qq",
        "language": "en",
        "genre": "rock",
        "type": 1,
        "is_finished": true,
        "play_count": 500
    })"),
                            "qq");

    CHECK(a.name == "Album X");
    CHECK(a.id == "al1");
    CHECK(a.cover == "http://x/a.jpg");  // via the 'image' alias
    REQUIRE(a.artists.size() == 1);
    CHECK(a.artists.at(0).name == "Solo Artist");  // 'artist' singular alias
    CHECK(a.songCount == "10");
    CHECK(a.publishTime == "2021-05-01");
    REQUIRE(a.songs.size() == 1);
    CHECK(a.songs.at(0).platform == "qq");  // fallback propagates into songs
    CHECK(a.description == "a record");
    CHECK(a.platform == "qq");
    CHECK(a.language == "en");
    CHECK(a.genre == "rock");
    CHECK(a.type == 1);
    CHECK(a.isFinished);
    CHECK(a.playCount == "500");
}

TEST_CASE("album camelCase aliases for count/publish/play are honored") {
    auto a = parseAlbumInfo(parse(R"({
        "songCount": 3, "publishTime": "2020", "playCount": 8
    })"));
    CHECK(a.songCount == "3");
    CHECK(a.publishTime == "2020");
    CHECK(a.playCount == "8");
}

TEST_CASE("album platform falls back when absent or empty") {
    CHECK(parseAlbumInfo(parse(R"({"id":"1","name":"n"})"), "kg").platform ==
          "kg");
    CHECK(parseAlbumInfo(parse(R"({"platform":""})"), "kg").platform == "kg");
    CHECK(parseAlbumInfo(parse(R"({"platform":"netease"})"), "kg").platform ==
          "netease");
}

TEST_CASE(
    "artistNamesText joins an album's artists with ' / ', '-' when none") {
    // The discover album card reuses songArtistText's join logic via the shared
    // artistNamesText(vector<SongArtist>) helper. Assert it renders the album's
    // artist list the same way a song row does.
    auto multi = parseAlbumInfo(
        parse(R"({"id":"1","name":"n","artists":["Alice","Bob"]})"));
    CHECK(artistNamesText(multi.artists) == "Alice / Bob");

    auto none = parseAlbumInfo(parse(R"({"id":"1","name":"n"})"));
    CHECK(artistNamesText(none.artists) == "-");
}
