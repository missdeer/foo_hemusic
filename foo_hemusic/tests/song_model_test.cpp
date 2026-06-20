#include "api/song.h"

#include <string>

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::parseSongInfo;
using hemusic::parseSongList;
using hemusic::SongInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE(
    "parseSongInfo reads every field including nested album/artists/links") {
    auto v = parse(R"({
        "name": "Song A",
        "subtitle": "feat. X",
        "id": "1001",
        "duration": 240,
        "mv_id": "55",
        "album": {"name": "Album A", "id": "a1"},
        "artists": [{"id": "ar1", "name": "Alice"}, {"id": "ar2", "name": "Bob"}],
        "links": [{"name": "HQ", "quality": 320, "format": "mp3", "size": "8MB", "url": "http://x/1.mp3"}],
        "platform": "netease",
        "cover": "http://x/c.jpg",
        "original_type": 2,
        "path": "/local/a.mp3",
        "size": 8388608,
        "quality": "320",
        "alias": "the a-side"
    })");
    SongInfo s = parseSongInfo(v);

    CHECK(s.name == "Song A");
    CHECK(s.subtitle == "feat. X");
    CHECK(s.id == "1001");
    CHECK(s.duration == 240);
    CHECK(s.mvId == "55");
    REQUIRE(s.album.has_value());
    CHECK(s.album->name == "Album A");
    CHECK(s.album->id == "a1");
    REQUIRE(s.artists.size() == 2);
    CHECK(s.artists.at(0).name == "Alice");
    CHECK(s.artists.at(1).id == "ar2");
    REQUIRE(s.links.size() == 1);
    CHECK(s.links.at(0).quality == 320);
    CHECK(s.links.at(0).url == "http://x/1.mp3");
    CHECK(s.platform == "netease");
    CHECK(s.cover == "http://x/c.jpg");
    CHECK(s.originalType == 2);
    REQUIRE(s.path.has_value());
    CHECK(*s.path == "/local/a.mp3");
    REQUIRE(s.size.has_value());
    CHECK(*s.size == 8388608);
    CHECK(*s.quality == "320");
    CHECK(*s.alias == "the a-side");
}

TEST_CASE(
    "name falls back to title only on null/absent, never on empty string") {
    // The rule is Dart `??`: it coalesces null, not "". Encoding the
    // distinction matters because a backend that sends name:"" must NOT be
    // silently rescued by title -- that would mask a real data defect.
    SECTION("name absent -> use title") {
        auto s = parseSongInfo(parse(R"({"id":"1","title":"From Title"})"));
        CHECK(s.name == "From Title");
    }
    SECTION("name present and empty -> stays empty, title ignored") {
        auto s = parseSongInfo(
            parse(R"({"id":"1","name":"","title":"From Title"})"));
        CHECK(s.name.empty());
    }
    SECTION("name present wins over title") {
        auto s = parseSongInfo(
            parse(R"({"id":"1","name":"Real","title":"From Title"})"));
        CHECK(s.name == "Real");
    }
}

TEST_CASE("platform falls back to fallbackPlatform when absent or empty") {
    SECTION("absent") {
        auto s = parseSongInfo(parse(R"({"id":"1","name":"n"})"), "qq");
        CHECK(s.platform == "qq");
    }
    SECTION("empty string also falls back (Flutter checks isEmpty, not null)") {
        auto s = parseSongInfo(parse(R"({"id":"1","name":"n","platform":""})"),
                               "qq");
        CHECK(s.platform == "qq");
    }
    SECTION("present wins") {
        auto s = parseSongInfo(
            parse(R"({"id":"1","name":"n","platform":"netease"})"), "qq");
        CHECK(s.platform == "netease");
    }
}

TEST_CASE(
    "cover uses the first non-empty alias in cover/pic/imgurl/image/thumb") {
    // Precedence is load-bearing: the backend sends different keys per source,
    // and an earlier non-empty key must win over a later one.
    SECTION("falls through empties to a later alias") {
        auto s = parseSongInfo(
            parse(R"({"id":"1","name":"n","cover":"","pic":"http://p"})"));
        CHECK(s.cover == "http://p");
    }
    SECTION("earlier alias wins") {
        auto s = parseSongInfo(parse(
            R"({"id":"1","name":"n","cover":"http://c","image":"http://i"})"));
        CHECK(s.cover == "http://c");
    }
    SECTION("none present -> empty") {
        auto s = parseSongInfo(parse(R"({"id":"1","name":"n"})"));
        CHECK(s.cover.empty());
    }
}

TEST_CASE(
    "artists tolerate scalar lists, a single scalar, and the 'artist' alias") {
    SECTION("list of strings, empties dropped") {
        auto s = parseSongInfo(
            parse(R"({"id":"1","name":"n","artists":["Alice","","Bob"]})"));
        REQUIRE(s.artists.size() == 2);
        CHECK(s.artists.at(0).name == "Alice");
        CHECK(s.artists.at(1).name == "Bob");
    }
    SECTION("single scalar -> one artist") {
        auto s =
            parseSongInfo(parse(R"({"id":"1","name":"n","artists":"Solo"})"));
        REQUIRE(s.artists.size() == 1);
        CHECK(s.artists.at(0).name == "Solo");
    }
    SECTION("'artist' alias used when 'artists' absent") {
        auto s =
            parseSongInfo(parse(R"({"id":"1","name":"n","artist":"Aliased"})"));
        REQUIRE(s.artists.size() == 1);
        CHECK(s.artists.at(0).name == "Aliased");
    }
}

TEST_CASE("album accepts an object or a bare string, empty -> none") {
    SECTION("bare string -> name only") {
        auto s = parseSongInfo(
            parse(R"({"id":"1","name":"n","album":"Just A Name"})"));
        REQUIRE(s.album.has_value());
        CHECK(s.album->name == "Just A Name");
        CHECK(s.album->id.empty());
    }
    SECTION("empty string -> none") {
        auto s = parseSongInfo(parse(R"({"id":"1","name":"n","album":""})"));
        CHECK_FALSE(s.album.has_value());
    }
}

TEST_CASE("links drop entries with no quality and no url") {
    auto s = parseSongInfo(parse(R"({"id":"1","name":"n","links":[
        {"quality":0,"url":""},
        {"quality":128,"url":""},
        {"quality":0,"url":"http://only-url"}
    ]})"));
    // First entry is useless and dropped; the other two each satisfy one half
    // of the OR filter.
    REQUIRE(s.links.size() == 2);
    CHECK(s.links.at(0).quality == 128);
    CHECK(s.links.at(1).url == "http://only-url");
}

TEST_CASE("parseSongList drops songs missing an id or a name") {
    // A track with no id can't be played or keyed, and an unnamed one can't be
    // shown -- both are unusable, so the list filter removes them.
    auto v = parse(R"([
        {"id":"1","name":"Keep"},
        {"id":"","name":"NoId"},
        {"id":"2","name":""},
        {"id":"3","name":"AlsoKeep"}
    ])");
    auto songs = parseSongList(v, "netease");

    REQUIRE(songs.size() == 2);
    CHECK(songs.at(0).name == "Keep");
    CHECK(songs.at(1).name == "AlsoKeep");
    // fallbackPlatform propagates to the kept entries.
    CHECK(songs.at(0).platform == "netease");
}

TEST_CASE("sublist is parsed recursively with the same fallback platform") {
    auto s = parseSongInfo(parse(R"({
        "id":"1","name":"Parent",
        "sublist":[{"id":"2","name":"Child"},{"id":"","name":"Dropped"}]
    })"),
                           "qq");
    REQUIRE(s.sublist.size() == 1);
    CHECK(s.sublist.at(0).name == "Child");
    CHECK(s.sublist.at(0).platform == "qq");
}

TEST_CASE(
    "numeric fields coerce string-encoded numbers; nullable fields honor "
    "absence") {
    SECTION("duration as a numeric string") {
        auto s =
            parseSongInfo(parse(R"({"id":"1","name":"n","duration":"300"})"));
        CHECK(s.duration == 300);
    }
    SECTION("absent nullable fields are nullopt") {
        auto s = parseSongInfo(parse(R"({"id":"1","name":"n"})"));
        CHECK_FALSE(s.path.has_value());
        CHECK_FALSE(s.size.has_value());
        CHECK_FALSE(s.quality.has_value());
        CHECK_FALSE(s.alias.has_value());
    }
    SECTION("present-but-unparseable size keeps 0, not nullopt") {
        auto s = parseSongInfo(parse(R"({"id":"1","name":"n","size":"abc"})"));
        REQUIRE(s.size.has_value());
        CHECK(*s.size == 0);
    }
}
