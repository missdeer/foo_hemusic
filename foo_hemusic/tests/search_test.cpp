#include "api/search.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::AlbumInfo;
using hemusic::ArtistInfo;
using hemusic::ComprehensiveSearchResult;
using hemusic::kFeatureComprehensiveSearch;
using hemusic::MvInfo;
using hemusic::parseAlbumSearch;
using hemusic::parseArtistSearch;
using hemusic::parseComprehensiveSearch;
using hemusic::parsePlaylistSearch;
using hemusic::parseSongSearch;
using hemusic::parseVideoSearch;
using hemusic::PlatformInfo;
using hemusic::PlaylistInfo;
using hemusic::resolveSearchPlatform;
using hemusic::SongInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

// status==1 => available(); flag carries (or omits) the searchSong bit.
PlatformInfo plat(std::string id, long long status, unsigned long long flag) {
    PlatformInfo p;
    p.id = std::move(id);
    p.name = p.id;
    p.status = status;
    p.featureSupportFlag = flag;
    return p;
}

}  // namespace

TEST_CASE("resolveSearchPlatform returns nullopt for an empty list") {
    CHECK_FALSE(resolveSearchPlatform({}).has_value());
}

TEST_CASE("resolveSearchPlatform skips platforms lacking comprehensiveSearch") {
    // 'a' carries searchSong (1<<1) and 'b' searchAlbum (1<<2) -- per-type
    // flags, NOT comprehensiveSearch (1<<0), so neither qualifies.
    const std::vector<PlatformInfo> platforms = {plat("a", 1, 1ULL << 1),
                                                 plat("b", 1, 1ULL << 2)};
    CHECK_FALSE(resolveSearchPlatform(platforms).has_value());
}

TEST_CASE("resolveSearchPlatform skips supporting-but-unavailable platforms") {
    // Carries the comprehensiveSearch bit but status != 1, so available() is
    // false.
    const std::vector<PlatformInfo> platforms = {
        plat("a", 0, kFeatureComprehensiveSearch)};
    CHECK_FALSE(resolveSearchPlatform(platforms).has_value());
}

TEST_CASE("resolveSearchPlatform picks the first available searchable one") {
    // 'nope' has only searchSong (1<<1), not comprehensiveSearch, so it is
    // skipped in favour of the first comprehensiveSearch-capable platform.
    const std::vector<PlatformInfo> platforms = {
        plat("nope", 1, 1ULL << 1),
        plat("kuwo", 1, kFeatureComprehensiveSearch),
        plat("netease", 1, kFeatureComprehensiveSearch)};
    auto p = resolveSearchPlatform(platforms);
    REQUIRE(p.has_value());
    CHECK(p->id == "kuwo");
}

TEST_CASE("parseComprehensiveSearch types every section + paging meta") {
    auto r = parseComprehensiveSearch(parse(R"({
        "key": "jay",
        "song":     {"list": [{"id":"s1","name":"A"}], "has_more": true, "total_count": 9},
        "playlist": {"items": [{"id":"p1","name":"PL"}], "total_count": "3"},
        "album":    {"list": [{"id":"al1","name":"AL"}]},
        "mv":       {"list": [{"id":"m1","name":"MV"}]},
        "artist":   {"list": [{"id":"ar1","name":"AR"}]}
    })"),
                                      "netease", "jay");

    CHECK(r.keyword == "jay");
    REQUIRE(r.song.items.size() == 1);
    CHECK(r.song.items.at(0).name == "A");
    CHECK(r.song.items.at(0).platform ==
          "netease");  // request platform fallback
    CHECK(r.song.hasMore);
    CHECK(r.song.totalCount == 9);
    // 'items' key alias + numeric-string total_count
    REQUIRE(r.playlist.items.size() == 1);
    CHECK(r.playlist.items.at(0).name == "PL");
    CHECK(r.playlist.totalCount == 3);
    CHECK_FALSE(r.playlist.hasMore);  // absent -> false
    REQUIRE(r.album.items.size() == 1);
    REQUIRE(r.video.items.size() == 1);
    REQUIRE(r.artist.items.size() == 1);
    CHECK(r.artist.items.at(0).name == "AR");
}

TEST_CASE("comprehensive keyword echoes body key, falls back to request") {
    SECTION("present body key wins") {
        auto r = parseComprehensiveSearch(parse(R"({"key":"echoed"})"), "qq",
                                          "typed");
        CHECK(r.keyword == "echoed");
    }
    SECTION("absent body key -> request keyword") {
        auto r = parseComprehensiveSearch(parse(R"({})"), "qq", "typed");
        CHECK(r.keyword == "typed");
    }
    SECTION("present-but-empty key -> empty (matches `?? ` on null only)") {
        auto r =
            parseComprehensiveSearch(parse(R"({"key":""})"), "qq", "typed");
        CHECK(r.keyword.empty());
    }
}

TEST_CASE(
    "best match: primary first, then recommendations, typed by resource") {
    auto r = parseComprehensiveSearch(parse(R"({
        "best_match": {
            "primary": {"resourceType":"artist", "artist":{"id":"ar1","name":"Jay"}},
            "recommendations": [
                {"resourceType":"song", "song":{"id":"s1","name":"Song"}},
                {"resourceType":"unknown", "unknown":{"x":1}},
                {"resourceType":"album", "album":{"id":"al1","name":"Alb"}},
                {"resourceType":"mv"}
            ]
        }
    })"),
                                      "netease", "jay");

    REQUIRE(r.bestMatch.size() == 3);  // unknown type + empty-data mv dropped
    CHECK(r.bestMatch.at(0).resourceType == "artist");
    CHECK(std::get<ArtistInfo>(r.bestMatch.at(0).data).name == "Jay");
    CHECK(r.bestMatch.at(1).resourceType == "song");
    CHECK(std::get<SongInfo>(r.bestMatch.at(1).data).name == "Song");
    CHECK(r.bestMatch.at(2).resourceType == "album");
    CHECK(std::get<AlbumInfo>(r.bestMatch.at(2).data).name == "Alb");
}

TEST_CASE("best match supports the bestMatch camelCase alias") {
    auto r = parseComprehensiveSearch(parse(R"({
        "bestMatch": {"primary": {"resourceType":"song","song":{"id":"s1","name":"S"}}}
    })"),
                                      "qq", "k");
    REQUIRE(r.bestMatch.size() == 1);
    CHECK(r.bestMatch.at(0).resourceType == "song");
}

TEST_CASE("comprehensive sections degrade leniently") {
    SECTION("missing sections -> empty, no best match") {
        auto r = parseComprehensiveSearch(parse(R"({})"), "qq", "k");
        CHECK(r.song.items.empty());
        CHECK(r.bestMatch.empty());
    }
    SECTION("section list nested under data") {
        auto r = parseComprehensiveSearch(
            parse(R"({"song":{"data":{"list":[{"id":"s1","name":"A"}]}}})"),
            "qq", "k");
        REQUIRE(r.song.items.size() == 1);
        CHECK(r.song.items.at(0).name == "A");
    }
    SECTION("non-object array entries are skipped") {
        auto r = parseComprehensiveSearch(
            parse(R"({"song":{"list":["junk",{"id":"s1","name":"A"}]}})"), "qq",
            "k");
        REQUIRE(r.song.items.size() == 1);
    }
}

TEST_CASE("categorized /v1/{type}/search returns typed lists") {
    CHECK(parseSongSearch(parse(R"({"list":[{"id":"s1","name":"A"}]})"), "qq")
              .size() == 1);
    CHECK(
        parsePlaylistSearch(parse(R"({"list":[{"id":"p1","name":"P"}]})"), "qq")
            .size() == 1);
    CHECK(parseAlbumSearch(parse(R"({"list":[{"id":"a1","name":"A"}]})"), "qq")
              .size() == 1);
    CHECK(parseArtistSearch(parse(R"({"list":[{"id":"r1","name":"R"}]})"), "qq")
              .size() == 1);
    CHECK(parseVideoSearch(parse(R"({"list":[{"id":"m1","name":"M"}]})"), "qq")
              .size() == 1);

    SECTION("missing list -> empty; platform fallback applies") {
        CHECK(parseSongSearch(parse(R"({})"), "qq").empty());
        auto songs = parseSongSearch(
            parse(R"({"list":[{"id":"s1","name":"A"}]})"), "qq");
        CHECK(songs.at(0).platform == "qq");
    }
}
