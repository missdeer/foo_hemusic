#pragma once

#include <algorithm>
#include <cstddef>

#include "ui/pages/discover_layout.h"

// Pure layout math for the playlist detail page (HEMUSIC-15) and the album
// detail page (HEMUSIC-16). Mirrors discover_layout.h's split: this header
// positions the banner + song rows in *content coordinates* (pre-scroll); the
// page translates by -scrollY when painting and adds +scrollY when hit-testing.
// No Win32/D2D deps -> testable.
//
// Reuses LayoutRect / SectionLayout / LayoutMetrics from discover_layout.h
// (LayoutMetrics gained detailBannerHeight / detailBannerCoverSide /
// detailBannerGap / detailEnqueueBtnW / detailEnqueueBtnH, all `detail`
// prefixed so the discover/search layouts that ignore them stay readable).
//
// DetailLayout is generic: the album page binds it identically -- both pages
// show a cover-left / info-right banner with an enqueue button pinned to the
// bottom-right plus a song list. Only the banner text content differs (album
// shows artists + publish_time vs playlist's creator), and that's rendered
// outside this layout module.

namespace hemusic::ui {

struct DetailLayout {
    float contentHeight = 0;
    LayoutRect banner;       // full banner band (padding + height)
    LayoutRect bannerCover;  // square cover on the left
    LayoutRect bannerInfo;   // text region (title/creator/counts) on the right
    LayoutRect enqueueButton;  // bottom-right of bannerInfo, fixed size
    SectionLayout songs;  // index/title/artist rows, like discover song-list
};

// Lays out: top padding -> banner (cover left / info right / enqueue button
// pinned bottom-right of info) -> sectionGap -> song list -> bottom padding.
// Empty song list collapses the section (no title band, no rows). Banner is
// always present (it carries the playlist/album meta even when songs aren't
// loaded yet), so contentHeight >= banner area + 2*padding.
inline DetailLayout computeDetailLayout(std::size_t songCount, float width,
                                        const LayoutMetrics& m) {
    const float avail = std::max(0.0F, width - 2.0F * m.padding);
    const float left = m.padding;

    DetailLayout out;

    // --- Banner ---
    const float bannerTop = m.padding;
    const float bannerBottom = bannerTop + m.detailBannerHeight;
    out.banner = {left, bannerTop, left + avail, bannerBottom};

    const float coverSide = std::min(m.detailBannerCoverSide, avail);
    out.bannerCover = {left, bannerTop, left + coverSide,
                       bannerTop + coverSide};

    const float infoLeft =
        std::min(out.bannerCover.right + m.detailBannerGap, left + avail);
    out.bannerInfo = {infoLeft, bannerTop, left + avail, bannerBottom};

    // Enqueue button pinned bottom-right of the info area; clamp so it never
    // escapes the banner even on a very narrow panel.
    const float btnW = std::min(m.detailEnqueueBtnW,
                                out.bannerInfo.right - out.bannerInfo.left);
    const float btnH = std::min(m.detailEnqueueBtnH,
                                out.bannerInfo.bottom - out.bannerInfo.top);
    const float btnRight = out.bannerInfo.right;
    const float btnBottom = out.bannerInfo.bottom;
    out.enqueueButton = {btnRight - btnW, btnBottom - btnH, btnRight,
                         btnBottom};

    float y = bannerBottom + m.sectionGap;

    // --- Songs section (collapses when empty) ---
    if (songCount > 0) {
        out.songs.present = true;
        out.songs.title = {left, y, left + avail, y + m.titleBand};
        y += m.titleBand;
        out.songs.items.reserve(songCount);
        for (std::size_t i = 0; i < songCount; ++i) {
            out.songs.items.push_back({left, y, left + avail, y + m.rowHeight});
            y += m.rowHeight;
        }
        y += m.sectionGap;
        // y carries one trailing sectionGap; swap for bottom padding.
        out.contentHeight = y - m.sectionGap + m.padding;
    } else {
        // No song list -> banner is the entire content; cancel the sectionGap
        // we tacked on above and add bottom padding only.
        out.contentHeight = bannerBottom + m.padding;
    }

    return out;
}

}  // namespace hemusic::ui
