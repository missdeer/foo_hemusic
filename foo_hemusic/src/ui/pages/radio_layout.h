#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "ui/pages/discover_layout.h"

// Pure layout math for the radio browse page (HEMUSIC-36). N dynamic groups,
// each rendered as a section title + a wrapped square-card grid; computed in
// content coordinates (pre-scroll), like discover_layout. No SDK / D2D / Win32
// deps -> unit-testable.

namespace hemusic::ui {

struct RadioLayout {
    float contentHeight = 0;            // 0 when every group is empty
    std::vector<SectionLayout> groups;  // one entry per input group, in order
};

// Lays out a list of group sizes top-to-bottom: each non-empty group becomes
// a title band + wrapped square card grid (album/playlist-style 1:1 cover +
// short text band). Empty groups collapse (no title, no space). Reuses the
// list/grid primitives from discover_layout's layout_detail namespace so the
// math + spacing match the rest of the app.
inline RadioLayout computeRadioLayout(
    const std::vector<std::size_t>& groupSizes, float width,
    const LayoutMetrics& m) {
    const float avail = std::max(0.0F, width - 2.0F * m.padding);
    const float left = m.padding;
    float y = m.padding;

    RadioLayout out;
    out.groups.resize(groupSizes.size());
    bool any = false;
    for (std::size_t i = 0; i < groupSizes.size(); ++i) {
        layout_detail::gridSection(out.groups.at(i), groupSizes.at(i), left,
                                   avail, y, m, m.squareCardWidth,
                                   /*square=*/true);
        if (out.groups.at(i).present) {
            any = true;
        }
    }
    // gridSection trails one sectionGap past the last present group; swap it
    // for the bottom padding (same trick as computeLayout).
    out.contentHeight = any ? (y - m.sectionGap + m.padding) : 0.0F;
    return out;
}

}  // namespace hemusic::ui
