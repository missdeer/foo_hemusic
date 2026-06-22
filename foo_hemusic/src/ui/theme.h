#pragma once

// Theme snapshot for the HE-Music panel (PLAN.md Phase 4: ui/theme.{h,cpp}).
//
// Per PLAN §3.5 the panel follows the fb2k host theme for colors + fonts (so it
// tracks the user's light/dark switch) while the *layout* mirrors the official
// site. This pulls the host's background / text / selection colors and default
// font family through the ui_element callback, blends a dimmed "secondary"
// text, and bundles the HE-Music layout constants every page draws against.
//
// Cheap to rebuild, so callers reconstruct it each paint (it reflects whatever
// theme is live then) rather than caching + invalidating on colors_changed.
//
// The fb2k callback type is forward-declared (SDK headers aren't self-contained
// and must follow <SDK/foobar2000.h>); only the .cpp pulls the SDK in.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include <string>

class ui_element_instance_callback;

namespace hemusic::ui {

// COLORREF (0x00BBGGRR) -> opaque D2D color. Shared by every custom-drawn
// widget; lives here as the single conversion point.
inline D2D1_COLOR_F toColorF(COLORREF c) {
    constexpr float kScale = 255.0F;
    return D2D1::ColorF(static_cast<float>(GetRValue(c)) / kScale,
                        static_cast<float>(GetGValue(c)) / kScale,
                        static_cast<float>(GetBValue(c)) / kScale, 1.0F);
}

namespace theme_detail {
// HE-Music layout constants (DIPs). First-pass values aligned loosely with
// y.wjhe.top list rows; not pixel-exact.
inline constexpr float kPadding = 12.0F;
inline constexpr float kSectionTitleSize = 20.0F;
inline constexpr float kRowHeight = 44.0F;
inline constexpr float kRowTitleSize = 15.0F;
inline constexpr float kRowSubSize = 12.0F;
// Fraction the secondary text is blended from text toward background.
inline constexpr float kSecondaryBlend = 0.45F;
}  // namespace theme_detail

struct Theme {
    // Colors sourced from the fb2k host (defaults used only when no callback).
    D2D1_COLOR_F background = D2D1::ColorF(D2D1::ColorF::White);
    D2D1_COLOR_F text = D2D1::ColorF(D2D1::ColorF::Black);
    D2D1_COLOR_F secondaryText = D2D1::ColorF(D2D1::ColorF::Gray);
    D2D1_COLOR_F selection = D2D1::ColorF(D2D1::ColorF::DodgerBlue);

    // Default UI font family (face name only; size is per-element below).
    std::wstring fontFamily = L"Segoe UI";

    float padding = theme_detail::kPadding;
    float sectionTitleSize = theme_detail::kSectionTitleSize;
    float rowHeight = theme_detail::kRowHeight;
    float rowTitleSize = theme_detail::kRowTitleSize;
    float rowSubSize = theme_detail::kRowSubSize;
};

// Builds a Theme from the host callback's live colors + default font. cb may be
// null (returns defaults). Call on the UI thread.
Theme themeFromCallback(ui_element_instance_callback* cb);

// Builds a Theme from the global ui_config_manager (Default UI) for standalone
// windows that have no ui_element callback -- e.g. the login dialog. Falls back
// to system colors per element, and to plain defaults when the service is
// absent (fb2k < 2.0). Call on the UI thread.
Theme themeFromHost();

}  // namespace hemusic::ui
