#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Pure layout math for the discover page's four stacked sections (HEMUSIC-13).
// No SDK / Direct2D / Win32 deps, so it is unit-testable in hemusic_tests:
// computeLayout positions section titles + items in *content coordinates*
// (before the scroll offset is applied) and reports the total content height.
// The drawing code (discover_page.cpp) translates each rect by -scrollY and
// derives a card's cover/title sub-rects from its rect. Own LayoutRect (not
// D2D1_RECT_F) keeps this header free of <d2d1.h>/<windows.h>; the layouts are
// field-compatible, so paint converts with D2D1::RectF(r.left, r.top, ...).

namespace hemusic::ui {

struct LayoutRect {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};

struct SectionLayout {
    bool present = false;  // false when the section is empty (collapsed)
    LayoutRect title;      // section title band (valid only if present)
    std::vector<LayoutRect> items;  // song rows or grid cards, in data order
};

struct DiscoverLayout {
    float contentHeight = 0;  // 0 when every section is empty
    SectionLayout songs;
    SectionLayout albums;
    SectionLayout playlists;
    SectionLayout mvs;
};

namespace layout_defaults {
// First-pass DIP metrics, loosely aligned with y.wjhe.top; not pixel-exact.
inline constexpr float kPadding = 12.0F;
inline constexpr float kTitleBand = 32.0F;
inline constexpr float kSectionGap = 18.0F;
inline constexpr float kRowHeight = 44.0F;
inline constexpr float kGridGap = 12.0F;
inline constexpr float kSquareCardWidth = 160.0F;
inline constexpr float kSquareCardTextH = 46.0F;
inline constexpr float kVideoCardWidth = 300.0F;
inline constexpr float kVideoAspect = 9.0F / 16.0F;
inline constexpr float kVideoCardTextH = 40.0F;
}  // namespace layout_defaults

struct LayoutMetrics {
    float padding = layout_defaults::kPadding;      // outer + scroll-bottom pad
    float titleBand = layout_defaults::kTitleBand;  // section title height
    float sectionGap = layout_defaults::kSectionGap;
    float rowHeight = layout_defaults::kRowHeight;  // song list row
    float gridGap = layout_defaults::kGridGap;  // gap between cards (both axes)
    float squareCardWidth =
        layout_defaults::kSquareCardWidth;  // album/playlist
    float squareCardTextH =
        layout_defaults::kSquareCardTextH;  // text below cover
    float videoCardWidth = layout_defaults::kVideoCardWidth;  // mv cards
    float videoAspect = layout_defaults::kVideoAspect;        // cover h / w
    float videoCardTextH = layout_defaults::kVideoCardTextH;
};

// Largest column count whose cards (>= targetWidth each, `gap` between) fit in
// availableWidth; never below 1. cols = floor((avail+gap)/(target+gap)).
inline int gridColumns(float availableWidth, float targetWidth, float gap) {
    if (availableWidth <= 0 || targetWidth <= 0) {
        return 1;
    }
    const int cols =
        static_cast<int>((availableWidth + gap) / (targetWidth + gap));
    return cols < 1 ? 1 : cols;
}

namespace layout_detail {

// Appends a full-width-row (song list) section. Advances y past it + gap.
inline void listSection(SectionLayout& s, std::size_t count, float left,
                        float avail, float& y, const LayoutMetrics& m) {
    if (count == 0) {
        return;
    }
    s.present = true;
    s.title = {left, y, left + avail, y + m.titleBand};
    y += m.titleBand;
    s.items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        s.items.push_back({left, y, left + avail, y + m.rowHeight});
        y += m.rowHeight;
    }
    y += m.sectionGap;
}

// Appends a wrapped card-grid section. `square` picks album/playlist (cover =
// card width) vs mv (16:9 cover). Advances y past it + gap.
inline void gridSection(SectionLayout& s, std::size_t count, float left,
                        float avail, float& y, const LayoutMetrics& m,
                        float targetWidth, bool square) {
    if (count == 0) {
        return;
    }
    s.present = true;
    s.title = {left, y, left + avail, y + m.titleBand};
    y += m.titleBand;

    const int cols = gridColumns(avail, targetWidth, m.gridGap);
    const float cardW = (avail - static_cast<float>(cols - 1) * m.gridGap) /
                        static_cast<float>(cols);
    const float coverH = square ? cardW : cardW * m.videoAspect;
    const float textH = square ? m.squareCardTextH : m.videoCardTextH;
    const float cardH = coverH + textH;

    s.items.reserve(count);
    const float rowTop = y;
    for (std::size_t i = 0; i < count; ++i) {
        const int idx = static_cast<int>(i);
        const int col = idx % cols;
        const int row = idx / cols;
        const float x = left + static_cast<float>(col) * (cardW + m.gridGap);
        const float cy = rowTop + static_cast<float>(row) * (cardH + m.gridGap);
        s.items.push_back({x, cy, x + cardW, cy + cardH});
    }
    const int rows = (static_cast<int>(count) + cols - 1) / cols;
    y = rowTop + static_cast<float>(rows) * cardH +
        static_cast<float>(rows - 1) * m.gridGap + m.sectionGap;
}

}  // namespace layout_detail

// Lays out songs -> albums -> playlists -> mvs (PLAN order). Empty sections are
// collapsed (no title, no space). contentHeight is 0 when everything is empty.
inline DiscoverLayout computeLayout(std::size_t songs, std::size_t albums,
                                    std::size_t playlists, std::size_t mvs,
                                    float width, const LayoutMetrics& m) {
    const float avail = std::max(0.0F, width - 2.0F * m.padding);
    const float left = m.padding;
    float y = m.padding;

    DiscoverLayout out;
    layout_detail::listSection(out.songs, songs, left, avail, y, m);
    layout_detail::gridSection(out.albums, albums, left, avail, y, m,
                               m.squareCardWidth, /*square=*/true);
    layout_detail::gridSection(out.playlists, playlists, left, avail, y, m,
                               m.squareCardWidth, /*square=*/true);
    layout_detail::gridSection(out.mvs, mvs, left, avail, y, m,
                               m.videoCardWidth, /*square=*/false);

    const bool any = out.songs.present || out.albums.present ||
                     out.playlists.present || out.mvs.present;
    // y carries one trailing sectionGap from the last present section; swap it
    // for the bottom padding.
    out.contentHeight = any ? (y - m.sectionGap + m.padding) : 0.0F;
    return out;
}

}  // namespace hemusic::ui
