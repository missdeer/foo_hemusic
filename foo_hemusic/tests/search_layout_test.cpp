#include "ui/pages/search_layout.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using hemusic::ui::computeSearchLayout;
using hemusic::ui::LayoutMetrics;
using hemusic::ui::LayoutRect;
using hemusic::ui::SearchLayout;

namespace {

constexpr float kEps = 0.01F;
constexpr float kWidth = 400.0F;      // typical panel width for layout checks
constexpr float kWideWidth = 800.0F;  // wide enough for multi-column video grid
constexpr float kZeroWidth = 0.0F;    // degenerate width (below 2*padding)

bool overlaps(const LayoutRect& a, const LayoutRect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom &&
           b.top < a.bottom;
}

}  // namespace

TEST_CASE("computeSearchLayout always lays out the keyword title at the top") {
    LayoutMetrics m;
    // Even with zero results the keyword title band sits at the top padding and
    // contentHeight is non-zero (the page only computes this with a keyword).
    auto l = computeSearchLayout(0, 0, 0, 0, 0, 0, kWidth, m);
    CHECK_THAT(l.keywordTitle.top, WithinAbs(m.padding, kEps));
    CHECK(l.keywordTitle.bottom > l.keywordTitle.top);
    CHECK(l.contentHeight > 0.0F);
    CHECK_FALSE(l.bestMatch.present);
    CHECK_FALSE(l.song.present);
    CHECK_FALSE(l.playlist.present);
    CHECK_FALSE(l.album.present);
    CHECK_FALSE(l.artist.present);
    CHECK_FALSE(l.video.present);
}

TEST_CASE(
    "sections stack in site order: best, song, playlist, album, artist, "
    "video") {
    LayoutMetrics m;
    auto l = computeSearchLayout(1, 1, 1, 1, 1, 1, kWidth, m);
    REQUIRE(l.bestMatch.present);
    REQUIRE(l.song.present);
    REQUIRE(l.playlist.present);
    REQUIRE(l.album.present);
    REQUIRE(l.artist.present);
    REQUIRE(l.video.present);
    // Keyword title is above every section.
    CHECK(l.keywordTitle.top < l.bestMatch.title.top);
    CHECK(l.bestMatch.title.top < l.song.title.top);
    CHECK(l.song.title.top < l.playlist.title.top);
    CHECK(l.playlist.title.top < l.album.title.top);
    CHECK(l.album.title.top < l.artist.title.top);
    CHECK(l.artist.title.top < l.video.title.top);
}

TEST_CASE("empty leading section does not reserve space below the keyword") {
    LayoutMetrics m;
    // No best-match / song: the playlist grid starts right below the keyword
    // title band, not below phantom best-match / song bands.
    auto l = computeSearchLayout(0, 0, 2, 0, 0, 0, kWidth, m);
    CHECK_FALSE(l.bestMatch.present);
    CHECK_FALSE(l.song.present);
    REQUIRE(l.playlist.present);
    CHECK_THAT(l.playlist.title.top,
               WithinAbs(m.padding + m.titleBand + m.sectionGap, kEps));
}

TEST_CASE("best-match rows stack contiguously without overlap") {
    LayoutMetrics m;
    auto l = computeSearchLayout(3, 0, 0, 0, 0, 0, kWidth, m);
    REQUIRE(l.bestMatch.present);
    REQUIRE(l.bestMatch.items.size() == 3);
    const auto& it = l.bestMatch.items;
    for (std::size_t i = 1; i < it.size(); ++i) {
        CHECK_THAT(it.at(i).top, WithinAbs(it.at(i - 1).bottom, kEps));
        CHECK_FALSE(overlaps(it.at(i - 1), it.at(i)));
    }
}

TEST_CASE("video cards in a row do not overlap and wrap to new rows") {
    LayoutMetrics m;
    // Wide enough that the 300-DIP video cards form >1 column, so 4 cards wrap.
    auto l = computeSearchLayout(0, 0, 0, 0, 0, 4, kWideWidth, m);
    REQUIRE(l.video.present);
    REQUIRE(l.video.items.size() == 4);
    const auto& it = l.video.items;
    CHECK_FALSE(overlaps(it.at(0), it.at(1)));
    CHECK(it.at(0).right <= it.at(1).left + kEps);
    CHECK_THAT(it.at(0).top, WithinAbs(it.at(1).top, kEps));
    CHECK_FALSE(overlaps(it.at(0), it.at(2)));
    CHECK(it.at(2).top > it.at(0).bottom - kEps);
}

TEST_CASE("search contentHeight grows strictly with more song rows") {
    LayoutMetrics m;
    const float h1 =
        computeSearchLayout(0, 1, 0, 0, 0, 0, kWidth, m).contentHeight;
    const float h5 =
        computeSearchLayout(0, 5, 0, 0, 0, 0, kWidth, m).contentHeight;
    CHECK(h5 > h1);
    // 5 rows vs 1 row => four extra rowHeights.
    const float extraRows = 4.0F;
    CHECK_THAT(h5 - h1, WithinAbs(extraRows * m.rowHeight, kEps));
}

TEST_CASE("computeSearchLayout tolerates a zero / tiny width") {
    LayoutMetrics m;
    auto l = computeSearchLayout(0, 0, 1, 0, 0, 0, kZeroWidth, m);
    REQUIRE(l.playlist.present);
    for (const auto& r : l.playlist.items) {
        CHECK(r.right >= r.left - kEps);
        CHECK(r.bottom >= r.top - kEps);
    }
}
