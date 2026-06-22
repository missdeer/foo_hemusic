#include "ui/pages/discover_layout.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using hemusic::ui::computeLayout;
using hemusic::ui::DiscoverLayout;
using hemusic::ui::gridColumns;
using hemusic::ui::LayoutMetrics;
using hemusic::ui::LayoutRect;

namespace {

constexpr float kEps = 0.01F;

bool overlaps(const LayoutRect& a, const LayoutRect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom &&
           b.top < a.bottom;
}

}  // namespace

TEST_CASE("gridColumns floors to fit and never drops below 1") {
    const float gap = 12.0F;
    // floor((avail+gap)/(target+gap)).
    CHECK(gridColumns(480.0F, 160.0F, gap) == 2);  // floor(492/172)=2
    CHECK(gridColumns(692.0F, 160.0F, gap) == 4);  // floor(704/172)=4
    CHECK(gridColumns(160.0F, 160.0F, gap) == 1);  // exactly one card
    // Degenerate inputs collapse to a single column instead of 0 / div-by-zero.
    CHECK(gridColumns(100.0F, 160.0F, gap) == 1);  // too narrow for one target
    CHECK(gridColumns(0.0F, 160.0F, gap) == 1);
    CHECK(gridColumns(480.0F, 0.0F, gap) == 1);
}

TEST_CASE("computeLayout with everything empty has zero height, no sections") {
    LayoutMetrics m;
    auto l = computeLayout(0, 0, 0, 0, 400.0F, m);
    CHECK(l.contentHeight == 0.0F);
    CHECK_FALSE(l.songs.present);
    CHECK_FALSE(l.albums.present);
    CHECK_FALSE(l.playlists.present);
    CHECK_FALSE(l.mvs.present);
}

TEST_CASE("song rows stack contiguously without overlap") {
    LayoutMetrics m;
    auto l = computeLayout(3, 0, 0, 0, 400.0F, m);
    REQUIRE(l.songs.present);
    REQUIRE(l.songs.items.size() == 3);
    // Title sits at the top padding; first row right below it.
    CHECK_THAT(l.songs.title.top, WithinAbs(m.padding, kEps));
    CHECK_THAT(l.songs.items[0].top, WithinAbs(m.padding + m.titleBand, kEps));
    // Each row's top meets the previous row's bottom exactly (no gap/overlap).
    for (std::size_t i = 1; i < l.songs.items.size(); ++i) {
        CHECK_THAT(l.songs.items[i].top,
                   WithinAbs(l.songs.items[i - 1].bottom, kEps));
        CHECK_FALSE(overlaps(l.songs.items[i - 1], l.songs.items[i]));
    }
    // Other sections collapsed.
    CHECK_FALSE(l.albums.present);
}

TEST_CASE("contentHeight grows strictly with more song rows") {
    LayoutMetrics m;
    const float h1 = computeLayout(1, 0, 0, 0, 400.0F, m).contentHeight;
    const float h5 = computeLayout(5, 0, 0, 0, 400.0F, m).contentHeight;
    CHECK(h5 > h1);
    // Each extra row adds exactly one rowHeight.
    CHECK_THAT(h5 - h1, WithinAbs(4.0F * m.rowHeight, kEps));
}

TEST_CASE("empty leading section does not reserve space") {
    LayoutMetrics m;
    // No songs: the albums grid must start at the top padding, not below a
    // phantom songs band.
    auto l = computeLayout(0, 2, 0, 0, 400.0F, m);
    CHECK_FALSE(l.songs.present);
    REQUIRE(l.albums.present);
    CHECK_THAT(l.albums.title.top, WithinAbs(m.padding, kEps));
}

TEST_CASE("grid cards in a row do not overlap and wrap to new rows") {
    LayoutMetrics m;
    // avail = 400-24 = 376; cols = floor(388/172) = 2; 4 cards => 2 rows.
    auto l = computeLayout(0, 4, 0, 0, 400.0F, m);
    REQUIRE(l.albums.present);
    REQUIRE(l.albums.items.size() == 4);
    const auto& it = l.albums.items;
    CHECK_FALSE(overlaps(it[0], it[1]));  // same row, side by side
    CHECK(it[0].right <= it[1].left + kEps);
    CHECK_THAT(it[0].top, WithinAbs(it[1].top, kEps));  // same row top
    CHECK_FALSE(overlaps(it[0], it[2]));                // card 2 wraps below 0
    CHECK(it[2].top > it[0].bottom - kEps);
}

TEST_CASE("sections stack in PLAN order: songs, albums, playlists, mvs") {
    LayoutMetrics m;
    auto l = computeLayout(1, 1, 1, 1, 400.0F, m);
    REQUIRE(l.songs.present);
    REQUIRE(l.albums.present);
    REQUIRE(l.playlists.present);
    REQUIRE(l.mvs.present);
    CHECK(l.songs.title.top < l.albums.title.top);
    CHECK(l.albums.title.top < l.playlists.title.top);
    CHECK(l.playlists.title.top < l.mvs.title.top);
}

TEST_CASE(
    "computeLayout tolerates a zero / tiny width without negative rects") {
    LayoutMetrics m;
    auto l = computeLayout(0, 3, 0, 0, 0.0F, m);  // width below 2*padding
    REQUIRE(l.albums.present);
    // availableWidth clamps to 0, columns collapse to 1; no negative geometry.
    for (const auto& r : l.albums.items) {
        CHECK(r.right >= r.left - kEps);
        CHECK(r.bottom >= r.top - kEps);
    }
}
