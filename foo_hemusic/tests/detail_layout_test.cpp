#include "ui/pages/detail_layout.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using hemusic::ui::computeDetailLayout;
using hemusic::ui::DetailLayout;
using hemusic::ui::LayoutMetrics;
using hemusic::ui::LayoutRect;

namespace {

constexpr float kEps = 0.01F;

bool overlaps(const LayoutRect& a, const LayoutRect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom &&
           b.top < a.bottom;
}

bool contains(const LayoutRect& outer, const LayoutRect& inner) {
    return inner.left >= outer.left - kEps &&
           inner.right <= outer.right + kEps && inner.top >= outer.top - kEps &&
           inner.bottom <= outer.bottom + kEps;
}

}  // namespace

TEST_CASE("detail layout with no songs keeps banner + collapses song section") {
    LayoutMetrics m;
    auto l = computeDetailLayout(0, 400.0F, m);
    CHECK(l.banner.right > l.banner.left);
    CHECK(l.bannerCover.right > l.bannerCover.left);
    CHECK(l.bannerInfo.right > l.bannerInfo.left);
    CHECK(l.enqueueButton.right > l.enqueueButton.left);
    CHECK_FALSE(l.songs.present);
    CHECK(l.songs.items.empty());
    // contentHeight = banner area (top padding + height) + bottom padding.
    CHECK_THAT(l.contentHeight,
               WithinAbs(m.padding + m.detailBannerHeight + m.padding, kEps));
}

TEST_CASE("detail layout song rows stack contiguously without overlap") {
    LayoutMetrics m;
    auto l = computeDetailLayout(4, 400.0F, m);
    REQUIRE(l.songs.present);
    REQUIRE(l.songs.items.size() == 4);
    // First row sits below banner + sectionGap + titleBand.
    const float expectedFirstTop =
        m.padding + m.detailBannerHeight + m.sectionGap + m.titleBand;
    CHECK_THAT(l.songs.items[0].top, WithinAbs(expectedFirstTop, kEps));
    for (std::size_t i = 1; i < l.songs.items.size(); ++i) {
        CHECK_THAT(l.songs.items[i].top,
                   WithinAbs(l.songs.items[i - 1].bottom, kEps));
        CHECK_FALSE(overlaps(l.songs.items[i - 1], l.songs.items[i]));
    }
}

TEST_CASE("detail layout contentHeight grows strictly with more songs") {
    LayoutMetrics m;
    const float h0 = computeDetailLayout(0, 400.0F, m).contentHeight;
    const float h1 = computeDetailLayout(1, 400.0F, m).contentHeight;
    const float h5 = computeDetailLayout(5, 400.0F, m).contentHeight;
    CHECK(h1 > h0);
    CHECK(h5 > h1);
    // Each extra row adds exactly one rowHeight.
    CHECK_THAT(h5 - h1, WithinAbs(4.0F * m.rowHeight, kEps));
}

TEST_CASE("detail layout tolerates zero width without negative rects") {
    LayoutMetrics m;
    auto l = computeDetailLayout(3, 0.0F, m);
    // Banner / cover / info / button all degrade but remain non-negative
    // (right >= left, bottom >= top).
    for (const LayoutRect* r :
         {&l.banner, &l.bannerCover, &l.bannerInfo, &l.enqueueButton}) {
        CHECK(r->right >= r->left - kEps);
        CHECK(r->bottom >= r->top - kEps);
    }
    REQUIRE(l.songs.present);
    for (const auto& row : l.songs.items) {
        CHECK(row.right >= row.left - kEps);
        CHECK(row.bottom >= row.top - kEps);
    }
}

TEST_CASE("enqueue button sits inside the banner info region, bottom-right") {
    LayoutMetrics m;
    auto l = computeDetailLayout(2, 600.0F, m);
    CHECK(contains(l.banner, l.bannerCover));
    CHECK(contains(l.banner, l.bannerInfo));
    CHECK(contains(l.bannerInfo, l.enqueueButton));
    // Pinned to the bottom-right corner of bannerInfo.
    CHECK_THAT(l.enqueueButton.right, WithinAbs(l.bannerInfo.right, kEps));
    CHECK_THAT(l.enqueueButton.bottom, WithinAbs(l.bannerInfo.bottom, kEps));
}

TEST_CASE("banner always starts at top padding (hit-test coord invariant)") {
    LayoutMetrics m;
    // Banner top is the anchor every hit-test relies on: yContent = yDip +
    // scrollY_, then check against banner.top == padding. Verify for both
    // empty and populated song lists.
    for (std::size_t n : {std::size_t(0), std::size_t(3), std::size_t(50)}) {
        auto l = computeDetailLayout(n, 800.0F, m);
        CHECK_THAT(l.banner.top, WithinAbs(m.padding, kEps));
        CHECK_THAT(l.bannerCover.top, WithinAbs(m.padding, kEps));
        CHECK_THAT(l.bannerInfo.top, WithinAbs(m.padding, kEps));
    }
}
