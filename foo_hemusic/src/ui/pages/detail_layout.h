#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

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

// Artist detail (HEMUSIC-17): banner (no enqueue button) + 3-tab strip +
// active-tab content area. The body's geometry depends on which tab is active
// (song list / square card grid / 16:9 card grid), so the layout takes the
// active tab + the active tab's item count and emits only that body.
enum class ArtistTab : std::uint8_t { Songs = 0, Albums = 1, Mvs = 2 };

struct ArtistTabBarLayout {
    LayoutRect band;                   // whole tab strip rect
    std::array<LayoutRect, 3> tabs{};  // each tab cell (3 equal-width slots)
};

struct ArtistDetailLayout {
    float contentHeight = 0;
    LayoutRect banner;
    LayoutRect bannerCover;
    LayoutRect bannerInfo;
    ArtistTabBarLayout tabBar;
    SectionLayout body;  // active tab's list or grid
};

// Lays out: padding -> banner -> sectionGap -> tab strip -> sectionGap ->
// active body -> bottom padding. Body collapses (no rows) when itemCount == 0
// but tab strip and banner stay so the user can switch tabs / read the meta.
inline ArtistDetailLayout computeArtistLayout(ArtistTab active,
                                              std::size_t itemCount,
                                              float width,
                                              const LayoutMetrics& m) {
    const float avail = std::max(0.0F, width - 2.0F * m.padding);
    const float left = m.padding;

    ArtistDetailLayout out;

    // --- Banner (cover left, info right; no enqueue button on artist) ---
    const float bannerTop = m.padding;
    const float bannerBottom = bannerTop + m.detailBannerHeight;
    out.banner = {left, bannerTop, left + avail, bannerBottom};
    const float coverSide = std::min(m.detailBannerCoverSide, avail);
    out.bannerCover = {left, bannerTop, left + coverSide,
                       bannerTop + coverSide};
    const float infoLeft =
        std::min(out.bannerCover.right + m.detailBannerGap, left + avail);
    out.bannerInfo = {infoLeft, bannerTop, left + avail, bannerBottom};

    float y = bannerBottom + m.sectionGap;

    // --- Tab strip: 3 equal-width slots over the same band as a section title.
    out.tabBar.band = {left, y, left + avail, y + m.titleBand};
    const float tabW = avail / 3.0F;
    for (std::size_t i = 0; i < out.tabBar.tabs.size(); ++i) {
        const float tx = left + static_cast<float>(i) * tabW;
        out.tabBar.tabs.at(i) = {tx, y, tx + tabW, y + m.titleBand};
    }
    y += m.titleBand + m.sectionGap;

    // --- Active body. listSection / gridSection both append a trailing
    //     sectionGap which we swap for the bottom padding below. When the
    //     active tab has 0 items the section is collapsed; we still pad.
    switch (active) {
        case ArtistTab::Songs:
            layout_detail::listSection(out.body, itemCount, left, avail, y, m);
            break;
        case ArtistTab::Albums:
            layout_detail::gridSection(out.body, itemCount, left, avail, y, m,
                                       m.squareCardWidth, /*square=*/true);
            break;
        case ArtistTab::Mvs:
            layout_detail::gridSection(out.body, itemCount, left, avail, y, m,
                                       m.videoCardWidth, /*square=*/false);
            break;
    }
    // listSection/gridSection both wrote (and added their own titleBand) only
    // if itemCount > 0; reset that here -- artist body has no separate title
    // since the tab strip already labels it.
    out.body.title = {};

    if (itemCount > 0) {
        // listSection/gridSection advanced y past the items + a trailing
        // sectionGap. Swap that trailing gap for bottom padding.
        out.contentHeight = y - m.sectionGap + m.padding;
        // Strip the title band the helpers tacked on top of the body, since
        // tab strip stands in for it: shift every item up by titleBand and
        // shrink contentHeight by the same amount.
        const float shift = m.titleBand;
        for (auto& it : out.body.items) {
            it.top -= shift;
            it.bottom -= shift;
        }
        out.contentHeight -= shift;
    } else {
        out.contentHeight = y - m.sectionGap + m.padding;
    }
    return out;
}

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
