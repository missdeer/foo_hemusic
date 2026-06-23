#include "api/playlist_square.h"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

using hemusic::CategoryInfo;
using hemusic::kPlaylistSquarePageSize;
using hemusic::parseCategoryPlaylistsPage;
using hemusic::parsePlaylistCategoryGroup;
using hemusic::parsePlaylistCategoryGroups;
using hemusic::PlaylistCategoryGroup;
using hemusic::PlaylistSquarePage;

namespace {

boost::json::value parse(const char* json) { return boost::json::parse(json); }

// Probe page sizes for the size-fallback test: smaller than the actual list
// (-> has_more true) and larger (-> false). Naming them satisfies the
// magic-number lint.
constexpr int kProbePageSizeBelow = 3;
constexpr int kProbePageSizeAbove = 5;

}  // namespace

TEST_CASE("parsePlaylistCategoryGroup reads name + categories") {
    SECTION("object categories with own platform + fallback for missing") {
        auto g =
            parsePlaylistCategoryGroup(parse(R"({"name":"风格","categories":[
                {"id":"c1","name":"流行","platform":"netease"},
                {"id":"c2","name":"民谣"}
            ]})"),
                                       "qq");
        REQUIRE(g.name == "风格");
        REQUIRE(g.categories.size() == 2);
        CHECK(g.categories.at(0).name == "流行");
        CHECK(g.categories.at(0).id == "c1");
        CHECK(g.categories.at(0).platform == "netease");
        CHECK(g.categories.at(1).name == "民谣");
        CHECK(g.categories.at(1).platform == "qq");
    }
    SECTION("scalar category items become bare-name categories with fallback") {
        auto g = parsePlaylistCategoryGroup(
            parse(R"({"name":"心情","categories":["  开心  ","难过"]})"), "qq");
        REQUIRE(g.categories.size() == 2);
        CHECK(g.categories.at(0).name == "开心");
        CHECK(g.categories.at(0).platform == "qq");
        CHECK(g.categories.at(1).name == "难过");
    }
    SECTION("empty-name categories are dropped") {
        auto g = parsePlaylistCategoryGroup(
            parse(
                R"({"name":"x","categories":[{"name":""},"   ",{"name":"K"}]})"),
            "qq");
        REQUIRE(g.categories.size() == 1);
        CHECK(g.categories.at(0).name == "K");
    }
    SECTION("missing categories yields empty list, not crash") {
        auto g = parsePlaylistCategoryGroup(parse(R"({"name":"x"})"), "qq");
        CHECK(g.categories.empty());
    }
}

TEST_CASE("parsePlaylistCategoryGroups handles top-level shapes") {
    SECTION("happy: 2 groups") {
        auto v = parsePlaylistCategoryGroups(parse(R"({"groups":[
                {"name":"A","categories":[{"id":"1","name":"a1"}]},
                {"name":"B","categories":[{"id":"2","name":"b1"}]}
            ]})"),
                                             "qq");
        REQUIRE(v.size() == 2);
        CHECK(v.at(0).name == "A");
        CHECK(v.at(1).categories.at(0).id == "2");
    }
    SECTION("missing groups -> empty (lenient)") {
        CHECK(parsePlaylistCategoryGroups(parse(R"({})"), "qq").empty());
    }
    SECTION("non-array groups -> empty (lenient)") {
        CHECK(parsePlaylistCategoryGroups(parse(R"({"groups":"oops"})"), "qq")
                  .empty());
    }
    SECTION("non-object group items are skipped") {
        auto v = parsePlaylistCategoryGroups(
            parse(R"({"groups":["junk",{"name":"OK"}]})"), "qq");
        REQUIRE(v.size() == 1);
        CHECK(v.at(0).name == "OK");
    }
}

TEST_CASE("parseCategoryPlaylistsPage parses list/has_more/last_id") {
    SECTION("explicit has_more + last_id wins over size fallback") {
        auto p = parseCategoryPlaylistsPage(
            parse(R"({"list":[{"id":"p1","name":"P1"}],
                      "has_more":true,"last_id":"L1"})"),
            "qq", kPlaylistSquarePageSize);
        REQUIRE(p.list.size() == 1);
        CHECK(p.list.at(0).id == "p1");
        CHECK(p.list.at(0).platform == "qq");  // fallback platform
        CHECK(p.hasMore == true);
        CHECK(p.lastId == "L1");
    }
    SECTION("missing has_more falls back to list.size >= pageSize") {
        const auto* payload = R"({"list":[
            {"id":"p1","name":"P1"},
            {"id":"p2","name":"P2"},
            {"id":"p3","name":"P3"}
        ]})";
        auto pFull = parseCategoryPlaylistsPage(parse(payload), "qq",
                                                kProbePageSizeBelow);
        CHECK(pFull.hasMore == true);
        auto pPart = parseCategoryPlaylistsPage(parse(payload), "qq",
                                                kProbePageSizeAbove);
        CHECK(pPart.hasMore == false);
    }
    SECTION("pageSize=0 disables the size fallback (has_more stays false)") {
        auto p = parseCategoryPlaylistsPage(
            parse(R"({"list":[{"id":"p1","name":"P1"}]})"), "qq",
            /*pageSize=*/0);
        CHECK(p.hasMore == false);
    }
    SECTION("missing list -> empty page, has_more=false, no crash") {
        auto p = parseCategoryPlaylistsPage(parse(R"({})"), "qq");
        CHECK(p.list.empty());
        CHECK(p.hasMore == false);
        CHECK(p.lastId.empty());
    }
    SECTION("non-object items in list are skipped") {
        auto p = parseCategoryPlaylistsPage(
            parse(R"({"list":["junk",{"id":"p1","name":"P1"}]})"), "qq");
        REQUIRE(p.list.size() == 1);
        CHECK(p.list.at(0).id == "p1");
    }
    SECTION("lastId camelCase alias") {
        auto p = parseCategoryPlaylistsPage(
            parse(R"({"list":[],"lastId":"abc"})"), "qq");
        CHECK(p.lastId == "abc");
    }
}

TEST_CASE("kPlaylistSquarePageSize matches Flutter default") {
    // Documenting the contract: Flutter playlist_plaza_api_client.dart default
    // (page_size = 30). If the backend changes its default, bump this
    // constant + the matching Flutter client.
    constexpr int kFlutterDefaultPageSize = 30;
    STATIC_REQUIRE(kPlaylistSquarePageSize == kFlutterDefaultPageSize);
}
