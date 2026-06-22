#pragma once

#include <algorithm>
#include <cstddef>

#include "ui/pages/discover_layout.h"

// Pure layout math for the comprehensive search page (HEMUSIC-14). Like
// discover_layout.h it has no SDK / Direct2D / Win32 deps, so it is
// unit-testable: computeSearchLayout positions a keyword-title band followed by
// the best-match + five typed sections in *content coordinates* (before the
// scroll offset). The page reuses discover_layout's LayoutRect / SectionLayout
// / LayoutMetrics and the listSection / gridSection helpers, so the two pages
// share one set of layout primitives.
//
// The non-scrolling search input box is chrome owned by the host panel
// (positioned above this content area), so it is deliberately NOT part of this
// layout -- computeSearchLayout starts at the keyword title.
//
// Section order mirrors the official site's comprehensive tab order:
// best-match -> song -> playlist -> album -> artist -> video. Best-match and
// song render as full-width list rows; playlist/album/artist as square card
// grids; video as a 16:9 card grid.

namespace hemusic::ui {

struct SearchLayout {
    float contentHeight = 0;  // always >= the keyword title + padding
    LayoutRect keywordTitle;  // "<keyword> 搜索结果" band (always present)
    SectionLayout bestMatch;
    SectionLayout song;
    SectionLayout playlist;
    SectionLayout album;
    SectionLayout artist;
    SectionLayout video;
};

// Lays out keyword title -> bestMatch -> song -> playlist -> album -> artist ->
// video. Empty sections collapse (no title, no space). The keyword title is
// always laid out, so contentHeight is never 0 (the page only computes this in
// the Loaded state, where a keyword exists).
inline SearchLayout computeSearchLayout(std::size_t bestMatch, std::size_t song,
                                        std::size_t playlist, std::size_t album,
                                        std::size_t artist, std::size_t video,
                                        float width, const LayoutMetrics& m) {
    const float avail = std::max(0.0F, width - 2.0F * m.padding);
    const float left = m.padding;
    float y = m.padding;

    SearchLayout out;
    out.keywordTitle = {left, y, left + avail, y + m.titleBand};
    y += m.titleBand + m.sectionGap;

    layout_detail::listSection(out.bestMatch, bestMatch, left, avail, y, m);
    layout_detail::listSection(out.song, song, left, avail, y, m);
    layout_detail::gridSection(out.playlist, playlist, left, avail, y, m,
                               m.squareCardWidth, /*square=*/true);
    layout_detail::gridSection(out.album, album, left, avail, y, m,
                               m.squareCardWidth, /*square=*/true);
    layout_detail::gridSection(out.artist, artist, left, avail, y, m,
                               m.squareCardWidth, /*square=*/true);
    layout_detail::gridSection(out.video, video, left, avail, y, m,
                               m.videoCardWidth, /*square=*/false);

    // y carries one trailing sectionGap (from the keyword title or the last
    // present section); swap it for the bottom padding.
    out.contentHeight = y - m.sectionGap + m.padding;
    return out;
}

}  // namespace hemusic::ui
