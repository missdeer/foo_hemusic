#include "api/discover.h"
#include "api/mv.h"
#include "api/platforms.h"

#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::DiscoverPage;
using hemusic::kFeatureGetDiscoverPage;
using hemusic::MvInfo;
using hemusic::parseDiscoverPage;
using hemusic::parseMvInfo;
using hemusic::PlatformInfo;
using hemusic::resolveDiscoverPlatform;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

// status==1 => available(); flag carries (or omits) the getDiscoverPage bit.
PlatformInfo plat(std::string id, long long status, unsigned long long flag) {
    PlatformInfo p;
    p.id = std::move(id);
    p.name = p.id;
    p.status = status;
    p.featureSupportFlag = flag;
    return p;
}

}  // namespace

TEST_CASE("parseMvInfo reads all fields and tolerates the play_count alias") {
    auto m = parseMvInfo(parse(R"({
        "platform": "netease",
        "links": [{"quality": 1080, "url": "http://x/mv.mp4"}],
        "id": "mv1",
        "name": "Some MV",
        "thumb": "http://x/t.jpg",
        "type": 2,
        "playCount": 9000,
        "creator": "Studio",
        "duration": 215,
        "description": "a clip"
    })"));

    CHECK(m.platform == "netease");
    REQUIRE(m.links.size() == 1);
    CHECK(m.links.at(0).quality == 1080);
    CHECK(m.id == "mv1");
    CHECK(m.name == "Some MV");
    CHECK(m.cover == "http://x/t.jpg");  // via the 'thumb' alias
    CHECK(m.type == 2);
    CHECK(m.playCount == "9000");  // camelCase alias honored
    CHECK(m.creator == "Studio");
    CHECK(m.duration == 215);
    CHECK(m.description == "a clip");
}

TEST_CASE("parseMvInfo platform falls back to the selected platform") {
    CHECK(parseMvInfo(parse(R"({"id":"1","name":"n"})"), "qq").platform ==
          "qq");
}

TEST_CASE(
    "parseDiscoverPage parses all four sections with the platform fallback") {
    auto page = parseDiscoverPage(parse(R"({
        "new_songs": [{"id":"s1","name":"Song"}],
        "new_albums": [{"id":"a1","name":"Album"}],
        "featured_playlists": [{"id":"p1","name":"Playlist"}],
        "featured_mvs": [{"id":"m1","name":"MV"}]
    })"),
                                  "netease");

    REQUIRE(page.newSongs.size() == 1);
    CHECK(page.newSongs.at(0).name == "Song");
    CHECK(page.newSongs.at(0).platform == "netease");  // fallback propagates

    REQUIRE(page.newAlbums.size() == 1);
    CHECK(page.newAlbums.at(0).name == "Album");
    CHECK(page.newAlbums.at(0).platform == "netease");

    REQUIRE(page.featuredPlaylists.size() == 1);
    CHECK(page.featuredPlaylists.at(0).name == "Playlist");
    CHECK(page.featuredPlaylists.at(0).platform == "netease");

    REQUIRE(page.featuredMvs.size() == 1);
    CHECK(page.featuredMvs.at(0).name == "MV");
    CHECK(page.featuredMvs.at(0).platform == "netease");
}

TEST_CASE("discover keeps every object entry in all four sections, no filter") {
    // Faithful to the datasource `_parseList`: unlike a nested Playlist/Album's
    // `_songs` (which drops id/name-less entries), the top-level discover
    // sections -- new_songs included -- keep every mapped object, even
    // malformed ones. The id/name filter must NOT leak up to this level.
    auto page = parseDiscoverPage(parse(R"({
        "new_songs": [{"id":"s1","name":"Keep"}, {"id":"","name":"NoId"}],
        "new_albums": [{"id":"","name":""}],
        "featured_playlists": [{"id":"","name":""}],
        "featured_mvs": [{"id":"","name":""}]
    })"),
                                  "netease");

    REQUIRE(page.newSongs.size() == 2);  // id-less song retained, not filtered
    CHECK(page.newSongs.at(0).name == "Keep");
    CHECK(page.newAlbums.size() == 1);  // kept despite empty id/name
    CHECK(page.featuredPlaylists.size() == 1);
    CHECK(page.featuredMvs.size() == 1);
}

TEST_CASE("discover skips non-object array items rather than keeping blanks") {
    // Flutter's `_asMap` would throw on a non-map item; the lenient parser
    // drops it instead of synthesizing an empty-shell entry.
    auto page = parseDiscoverPage(parse(R"({
        "new_songs": [{"id":"s1","name":"Real"}, 42, "junk", null],
        "new_albums": [{"id":"a1","name":"A"}, ["nested"]]
    })"),
                                  "qq");

    REQUIRE(page.newSongs.size() == 1);  // the three non-objects skipped
    CHECK(page.newSongs.at(0).name == "Real");
    REQUIRE(page.newAlbums.size() == 1);  // the array item skipped
    CHECK(page.newAlbums.at(0).name == "A");
}

TEST_CASE("discover degrades a missing or non-array section to an empty list") {
    // Unlike Flutter (which throws on a missing field), the pure parser yields
    // empty sections so a partial backend response still renders.
    auto page = parseDiscoverPage(parse(R"({
        "new_songs": [{"id":"s1","name":"Only Songs"}]
    })"),
                                  "qq");

    CHECK(page.newSongs.size() == 1);
    CHECK(page.newAlbums.empty());
    CHECK(page.featuredPlaylists.empty());
    CHECK(page.featuredMvs.empty());

    // A non-array section is also tolerated.
    auto page2 = parseDiscoverPage(parse(R"({"new_albums": "oops"})"), "qq");
    CHECK(page2.newAlbums.empty());
}

TEST_CASE("resolveDiscoverPlatform returns nullopt for an empty list") {
    CHECK_FALSE(resolveDiscoverPlatform({}).has_value());
}

TEST_CASE("resolveDiscoverPlatform skips platforms lacking getDiscoverPage") {
    // Available but the discover bit is clear (only some other feature set).
    std::vector<PlatformInfo> platforms = {
        plat("a", 1, 1ULL << 0), plat("b", 1, kFeatureGetDiscoverPage >> 1)};
    CHECK_FALSE(resolveDiscoverPlatform(platforms).has_value());
}

TEST_CASE(
    "resolveDiscoverPlatform skips supporting-but-unavailable platforms") {
    // Carries the discover bit but status != 1, so available() is false.
    std::vector<PlatformInfo> platforms = {
        plat("a", 0, kFeatureGetDiscoverPage)};
    CHECK_FALSE(resolveDiscoverPlatform(platforms).has_value());
}

TEST_CASE(
    "resolveDiscoverPlatform picks the first available + supporting one") {
    // First two are disqualified (unavailable / unsupported); "c" qualifies and
    // "d" also qualifies but must not win -- first match, mirroring Flutter's
    // available.first.
    std::vector<PlatformInfo> platforms = {
        plat("a", 0, kFeatureGetDiscoverPage),       // unavailable
        plat("b", 1, 1ULL),                          // available, no discover
        plat("c", 1, kFeatureGetDiscoverPage | 1U),  // qualifies
        plat("d", 1, kFeatureGetDiscoverPage)};      // qualifies but later
    auto sel = resolveDiscoverPlatform(platforms);
    REQUIRE(sel.has_value());
    CHECK(sel->id == "c");
}
