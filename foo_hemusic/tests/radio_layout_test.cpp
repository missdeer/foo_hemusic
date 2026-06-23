#include "ui/pages/radio_layout.h"

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using hemusic::ui::computeRadioLayout;
using hemusic::ui::LayoutMetrics;
using hemusic::ui::LayoutRect;
using hemusic::ui::RadioLayout;
using hemusic::ui::SectionLayout;

namespace {

constexpr float kStdWidth = 800.0F;

bool overlaps(const LayoutRect& a, const LayoutRect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom &&
           b.top < a.bottom;
}

}  // namespace

TEST_CASE("computeRadioLayout: empty input -> zero height, no groups") {
    auto out = computeRadioLayout({}, kStdWidth, LayoutMetrics{});
    CHECK(out.groups.empty());
    CHECK(out.contentHeight == 0.0F);
}

TEST_CASE("computeRadioLayout: all groups empty -> zero height + collapsed") {
    std::vector<std::size_t> sizes{0, 0, 0};
    auto out = computeRadioLayout(sizes, kStdWidth, LayoutMetrics{});
    REQUIRE(out.groups.size() == 3);
    CHECK_FALSE(out.groups.at(0).present);
    CHECK_FALSE(out.groups.at(1).present);
    CHECK_FALSE(out.groups.at(2).present);
    CHECK(out.contentHeight == 0.0F);
}

TEST_CASE("computeRadioLayout: cards in one group do not overlap") {
    std::vector<std::size_t> sizes{4};
    auto out = computeRadioLayout(sizes, kStdWidth, LayoutMetrics{});
    REQUIRE(out.groups.size() == 1);
    const auto& items = out.groups.at(0).items;
    REQUIRE(items.size() == 4);
    CHECK_FALSE(overlaps(items.at(0), items.at(1)));
    CHECK_FALSE(overlaps(items.at(1), items.at(2)));
    CHECK_FALSE(overlaps(items.at(2), items.at(3)));
    CHECK(out.contentHeight > 0.0F);
}

TEST_CASE("computeRadioLayout: empty group between non-empty collapses") {
    std::vector<std::size_t> sizes{2, 0, 3};
    auto out = computeRadioLayout(sizes, kStdWidth, LayoutMetrics{});
    REQUIRE(out.groups.size() == 3);
    CHECK(out.groups.at(0).present);
    CHECK_FALSE(out.groups.at(1).present);
    CHECK(out.groups.at(2).present);
    const float g0Bottom = out.groups.at(0).items.back().bottom;
    const float g2Top = out.groups.at(2).title.top;
    CHECK(g2Top > g0Bottom);
}

TEST_CASE("computeRadioLayout: contentHeight grows with item count") {
    constexpr std::size_t kFewItems = 2;
    constexpr std::size_t kManyItems = 6;
    std::vector<std::size_t> few{kFewItems};
    std::vector<std::size_t> many{kManyItems};
    auto a = computeRadioLayout(few, kStdWidth, LayoutMetrics{});
    auto b = computeRadioLayout(many, kStdWidth, LayoutMetrics{});
    CHECK(b.contentHeight >= a.contentHeight);
}

TEST_CASE("computeRadioLayout: zero width degrades to a 1-column grid") {
    std::vector<std::size_t> sizes{3};
    auto out = computeRadioLayout(sizes, 0.0F, LayoutMetrics{});
    REQUIRE(out.groups.size() == 1);
    CHECK(out.groups.at(0).present);
    CHECK(out.groups.at(0).items.size() == 3);
    CHECK(out.contentHeight >= 0.0F);
}
