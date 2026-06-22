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

// Assembles a Theme from already-resolved host colors + font, shared by both
// the ui_element-callback and the global ui_config_manager paths.
Theme buildTheme(COLORREF background, COLORREF text, COLORREF selection,
                 HFONT font) {
    Theme theme;
    theme.background = toColorF(background);
    theme.text = toColorF(text);
    theme.selection = toColorF(selection);
    // No dedicated "secondary text" host color; dim the text toward the
    // background so artist/sub lines read as muted in both light and dark.
    theme.secondaryText =
        blend(theme.text, theme.background, theme_detail::kSecondaryBlend);

    // Default UI font: pull the face name off the host's HFONT so the panel
    // tracks the user's chosen family (PLAN §3.5). Size stays per-element.
    if (font != nullptr) {
        LOGFONTW lf{};
        if (GetObjectW(font, sizeof(lf), &lf) != 0 &&
            lf.lfFaceName[0] != L'\0') {
            theme.fontFamily = lf.lfFaceName;
        }
    }
    return theme;
}

}  // namespace

Theme themeFromCallback(ui_element_instance_callback* cb) {
    if (cb == nullptr) {
        return Theme{};
    }
    return buildTheme(cb->query_std_color(ui_color_background),
                      cb->query_std_color(ui_color_text),
                      cb->query_std_color(ui_color_selection),
                      cb->query_font_ex(ui_font_default));
}

Theme themeFromHost() {
    ui_config_manager::ptr cfg;
    if (!ui_config_manager::tryGet(cfg)) {
        return Theme{};
    }
    // query_color returns true only when the user overrode the element; fall
    // back to the mapped system color otherwise (mirrors query_std_color).
    auto stdColor = [&cfg](const GUID& what) -> COLORREF {
        t_ui_color c = 0;
        if (cfg->query_color(what, c)) {
            return c;
        }
        const int idx = ui_color_to_sys_color_index(what);
        return idx < 0 ? 0 : GetSysColor(idx);
    };
    return buildTheme(stdColor(ui_color_background), stdColor(ui_color_text),
                      stdColor(ui_color_selection),
                      cfg->query_font(ui_font_default));
}

}  // namespace hemusic::ui
