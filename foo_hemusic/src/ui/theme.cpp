// SDK headers first: foobar2000.h pulls in WinSock2 and sets the guard that
// stops the later <windows.h> (via theme.h) from re-including winsock.h v1.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDK/foobar2000.h>
#include <SDK/ui_element.h>

#include "ui/theme.h"

#include <windows.h>

namespace hemusic::ui {

namespace {

// Linear blend a -> b by t in [0,1].
float lerp(float a, float b, float t) { return a + (b - a) * t; }

D2D1_COLOR_F blend(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t),
                        1.0F);
}

}  // namespace

Theme themeFromCallback(ui_element_instance_callback* cb) {
    Theme theme;
    if (cb == nullptr) {
        return theme;
    }

    theme.background = toColorF(cb->query_std_color(ui_color_background));
    theme.text = toColorF(cb->query_std_color(ui_color_text));
    theme.selection = toColorF(cb->query_std_color(ui_color_selection));
    // No dedicated "secondary text" host color; dim the text toward the
    // background so artist/sub lines read as muted in both light and dark.
    theme.secondaryText =
        blend(theme.text, theme.background, theme_detail::kSecondaryBlend);

    // Default UI font: pull the face name off the host's HFONT so the panel
    // tracks the user's chosen family (PLAN §3.5). Size stays per-element.
    if (HFONT font = cb->query_font_ex(ui_font_default); font != nullptr) {
        LOGFONTW lf{};
        if (GetObjectW(font, sizeof(lf), &lf) != 0 &&
            lf.lfFaceName[0] != L'\0') {
            theme.fontFamily = lf.lfFaceName;
        }
    }
    return theme;
}

}  // namespace hemusic::ui
