#include "api/ranking.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::parseRankingDetail;
using hemusic::parseRankingGroup;
using hemusic::parseRankingGroups;
using hemusic::parseRankingInfo;
using hemusic::RankingDetail;
using hemusic::RankingGroup;
using hemusic::RankingInfo;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

}  // namespace

TEST_CASE("parseRankingInfo reads fields and builds preview songs") {
    auto r = parseRankingInfo(parse(R"({
        "id": "rk1",
        "platform": "netease",
        "title": "Hot 100",
        "pic": "http://x/c.jpg",
        "songs": [
            {"id":"s1","name":"A","artists":[{"name":"X"},{"name":"Y"}]},
            {"id":"s2","title":"B"},
            {"id":"s3","name":"C","artist":"Z"},
            {"id":"s4","name":"D"}
        ]
    })"),
                              "qq");

    CHECK(r.id == "rk1");
    CHECK(r.platform == "netease");         // explicit wins over fallback
    CHECK(r.name == "Hot 100");             // via 'title' alias
    CHECK(r.coverUrl == "http://x/c.jpg");  // via 'pic' alias
    REQUIRE(r.previewSongs.size() == 3);    // only first 3 taken
    CHECK(r.previewSongs.at(0).name == "A");
    CHECK(r.previewSongs.at(0).artist == "X / Y");  // joined
    CHECK(r.previewSongs.at(1).name == "B");        // via title
    CHECK(r.previewSongs.at(1).artist == "-");      // no artists -> "-"
    CHECK(r.previewSongs.at(2).artist == "Z");      // single 'artist' alias
}

TEST_CASE("parseRankingInfo applies id/platform/name fallbacks") {
    SECTION("empty id uses fallbackId, then '-'") {
        auto withFallback =
            parseRankingInfo(parse(R"({"name":"X"})"), "qq", "fid");
        CHECK(withFallback.id == "fid");
        auto noFallback = parseRankingInfo(parse(R"({"name":"X"})"), "qq");
        CHECK(noFallback.id == "-");
    }
    SECTION("empty platform/name use fallback/'-'") {
        auto r = parseRankingInfo(parse(R"({})"), "qq");
        CHECK(r.platform == "qq");
        CHECK(r.name == "-");
        CHECK(r.coverUrl.empty());
        CHECK(r.previewSongs.empty());
    }
}

TEST_CASE("ranking preview skips non-object songs without consuming a slot") {
    auto r = parseRankingInfo(parse(R"({
        "id": "rk1",
        "songs": ["junk", {"id":"s1","name":"A"}, {"id":"s2","name":"B"},
                  {"id":"s3","name":"C"}]
    })"),
                              "qq");
    // "junk" is skipped, so all three valid songs still fill the preview.
    REQUIRE(r.previewSongs.size() == 3);
    CHECK(r.previewSongs.at(0).name == "A");
    CHECK(r.previewSongs.at(2).name == "C");
}

TEST_CASE("parseRankingGroup names default and parse nested rankings") {
    SECTION("named group with rankings") {
        auto g = parseRankingGroup(parse(R"({
            "name": "Official",
            "rankings": [{"id":"rk1","name":"Top"}, "junk", {"id":"rk2","name":"New"}]
        })"),
                                   "qq");
        CHECK(g.name == "Official");
        REQUIRE(g.rankings.size() == 2);  // non-object 'junk' skipped
        CHECK(g.rankings.at(0).name == "Top");
        CHECK(g.rankings.at(0).platform == "qq");  // fallback propagates
        CHECK(g.rankings.at(1).name == "New");
    }
    SECTION("empty name defaults to the Chinese '榜单' literal") {
        auto g = parseRankingGroup(parse(R"({"rankings":[]})"), "qq");
        CHECK(g.name == "\xE6\xA6\x9C\xE5\x8D\x95");  // 榜单
    }
}

TEST_CASE("parseRankingGroups reads envelope and degrades leniently") {
    SECTION("well-formed") {
        auto groups = parseRankingGroups(parse(R"({
            "groups": [{"name":"G1","rankings":[]}, {"name":"G2","rankings":[]}]
        })"),
                                         "qq");
        REQUIRE(groups.size() == 2);
        CHECK(groups.at(0).name == "G1");
    }
    SECTION("missing/non-array/non-object body -> empty") {
        CHECK(parseRankingGroups(parse(R"({})"), "qq").empty());
        CHECK(parseRankingGroups(parse(R"({"groups":1})"), "qq").empty());
        CHECK(parseRankingGroups(parse("[]"), "qq").empty());
    }
}

TEST_CASE("parseRankingDetail reads info + songs + paging fields") {
    auto d = parseRankingDetail(parse(R"({
        "id": "rk1",
        "name": "Hot",
        "platform": "netease",
        "songs": [{"id":"s1","name":"A"}, {"id":"","name":"bad"}],
        "has_more": true,
        "last_id": "s1",
        "total_count": "120",
        "desc": "weekly chart"
    })"),
                                "qq", "reqid");

    CHECK(d.info.id == "rk1");
    CHECK(d.info.name == "Hot");
    REQUIRE(d.songs.size() == 1);  // empty-id song dropped (parseSongList)
    CHECK(d.songs.at(0).name == "A");
    // Songs fall back to the REQUEST platform ("qq"), not the chart's body
    // platform -- matching Flutter `_parseSongs(payload, platform)`.
    CHECK(d.songs.at(0).platform == "qq");
    CHECK(d.hasMore);
    CHECK(d.lastId == "s1");
    CHECK(d.totalCount == 120);              // numeric string parsed
    CHECK(d.description == "weekly chart");  // via 'desc' alias
}

TEST_CASE("ranking detail fallbacks: id, hasMore, totalCount edge cases") {
    SECTION("empty id falls back to the requested id") {
        auto d = parseRankingDetail(parse(R"({"name":"Hot"})"), "qq", "reqid");
        CHECK(d.info.id == "reqid");
    }
    SECTION("hasMore from camelCase + numeric, totalCount skips non-numeric") {
        auto d = parseRankingDetail(
            parse(R"({"hasMore":1,"totalCount":"oops"})"), "qq", "x");
        CHECK(d.hasMore);          // numeric 1 -> true
        CHECK(d.totalCount == 0);  // unparseable string -> 0
    }
    SECTION("defaults when absent") {
        auto d = parseRankingDetail(parse(R"({})"), "qq", "x");
        CHECK_FALSE(d.hasMore);
        CHECK(d.totalCount == 0);
        CHECK(d.lastId.empty());
        CHECK(d.songs.empty());
    }
}
