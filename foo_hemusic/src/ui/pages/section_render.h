#pragma once

// Shared Direct2D section rendering for the discover + search pages
// (HEMUSIC-13 / HEMUSIC-14). Both pages stack the same widgets -- a song list
// plus square/16:9 card grids -- over a scrollable content area, so the drawing
// primitives live here once instead of being duplicated per page. Layout math
// stays in discover_layout.h (pure, unit-tested); this unit only paints rects
// the layout produced. The Renderer translates content rects by -scrollY and
// culls off-viewport items; cover art is pulled from the shared cover cache and
// a placeholder box is drawn until the bitmap arrives.
//
// No SDK headers: this is plain Win32 + Direct2D + the header-only api/ models,
// so it links into the component DLL without pulling the foobar2000 SDK.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "api/song.h"
#include "net/http_client.h"
#include "ui/cover_cache.h"
#include "ui/d2d.h"
#include "ui/image_cache.h"
#include "ui/pages/discover_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

namespace render_detail {

inline constexpr long kHttpOkMin = 200;
inline constexpr long kHttpOkMax = 300;  // exclusive (2xx)

// Cover/data fetch timeouts (shorter than the 20s/30s default) so closing the
// panel mid-fetch bounds the worker join at teardown (review: AGY/Codex).
inline constexpr int kFetchConnectMs = 8000;
inline constexpr int kFetchReadMs = 15000;

// Song-row index column width (DIP) and card cover->text gap + related metrics.
inline constexpr float kIndexWidth = 28.0F;
inline constexpr float kCardInnerGap = 4.0F;
inline constexpr float kSubGap = 2.0F;  // title line -> sub line
inline constexpr float kVideoAspect = 9.0F / 16.0F;
inline constexpr float kStrokeWidth = 1.0F;
inline constexpr float kRowThumbPad =
    4.0F;  // song-row cover inset (top+bottom)

inline bool isHttpOk(const HttpResponse& r) {
    return r.ok && r.status >= kHttpOkMin && r.status < kHttpOkMax;
}

inline boost::json::value parseJson(const std::string& s) {
    boost::system::error_code ec;
    auto v = boost::json::parse(s, ec);
    return ec ? boost::json::value(nullptr) : v;
}

}  // namespace render_detail

// UTF-8 -> UTF-16 for Direct2D's DrawTextW.
std::wstring utf8ToWide(const std::string& s);

// Builds a no-wrap text format; `centered` centers both axes, `ellipsis` adds a
// trailing ellipsis trimming sign so overlong text can't spill across cells.
Microsoft::WRL::ComPtr<IDWriteTextFormat> makeFormat(const std::wstring& family,
                                                     float size, bool centered,
                                                     bool ellipsis);

// Clips text to its rect (D2D1_DRAW_TEXT_OPTIONS_CLIP) so a long title can't
// bleed past its cell into a neighbour.
void drawText(ID2D1RenderTarget* rt, IDWriteTextFormat* tf,
              const std::wstring& text, ID2D1Brush* brush,
              const D2D1_RECT_F& rect);

// Draws `bmp` to fill `dst` without distortion (center-crop / cover-fit).
void drawCoverBitmap(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp,
                     const D2D1_RECT_F& dst);

// Centered single-line message (loading / not-logged-in / error / empty).
void drawCentered(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size,
                  const std::wstring& text);

// Viewport height in DIPs, matching the units of the layout's contentHeight
// (which comes from rt->GetSize()). GetClientRect is physical pixels, so divide
// by the window's DPI scale -- otherwise high-DPI panels clamp the scroll too
// early and the bottom content stays unreachable. `topInsetDip` is subtracted
// for pages that reserve a band above the scrollable content (e.g. a tab bar).
float viewportHeightDip(HWND hwnd, float topInsetDip = 0.0F);

// Bundles the per-paint Direct2D resources + scroll/viewport so the section
// loops stay short. Translates content rects by -scrollY and culls off-viewport
// items.
struct Renderer {
    ID2D1RenderTarget* rt = nullptr;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> rowFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> subFmt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> subBrush;
    ImageCache* covers = nullptr;      // shared cover cache (may be null)
    const void* subscriber = nullptr;  // owner key for cover listeners
    HWND host = nullptr;               // posted to when a cover finishes
    UINT coverReadyMsg = 0;            // message posted on cover load
    float scrollY = 0;
    float viewportH = 0;
    float rowTitleH = 0;

    D2D1_RECT_F screen(const LayoutRect& r) const {
        return D2D1::RectF(r.left, r.top - scrollY, r.right,
                           r.bottom - scrollY);
    }
    bool visible(const D2D1_RECT_F& s) const {
        return s.bottom >= 0.0F && s.top <= viewportH;
    }
    void sectionTitle(const SectionLayout& s, const std::wstring& label) const {
        const D2D1_RECT_F sr = screen(s.title);
        if (visible(sr)) {
            drawText(rt, titleFmt.Get(), label, textBrush.Get(), sr);
        }
    }
    // Real cover if cached, else queues an async fetch and draws a placeholder
    // box until coverReadyMsg triggers a repaint. The makeBitmap upload is
    // per-paint (device-dependent resource, never cached across targets).
    void drawCover(const D2D1_RECT_F& dst, const std::string& url) const {
        if (covers != nullptr && !url.empty()) {
            const HWND h = host;
            const UINT msg = coverReadyMsg;
            Microsoft::WRL::ComPtr<IWICBitmapSource> src = covers->request(
                url, subscriber, [h, msg] { PostMessageW(h, msg, 0, 0); });
            if (src) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp =
                    d2d::makeBitmap(rt, src.Get());
                if (bmp) {
                    drawCoverBitmap(rt, bmp.Get(), dst);
                    return;
                }
            }
        }
        rt->DrawRectangle(dst, subBrush.Get(), render_detail::kStrokeWidth);
    }
    void songRow(const D2D1_RECT_F& r, int index, const std::wstring& name,
                 const std::wstring& artist,
                 const std::string& coverUrl) const {
        drawText(rt, subFmt.Get(), std::to_wstring(index), subBrush.Get(),
                 D2D1::RectF(r.left, r.top, r.left + render_detail::kIndexWidth,
                             r.bottom));
        const float thumbX = r.left + render_detail::kIndexWidth;
        const float side = std::max(
            0.0F, (r.bottom - r.top) - 2.0F * render_detail::kRowThumbPad);
        drawCover(
            D2D1::RectF(thumbX, r.top + render_detail::kRowThumbPad,
                        thumbX + side, r.bottom - render_detail::kRowThumbPad),
            coverUrl);
        const float tx = thumbX + side + render_detail::kCardInnerGap;
        const float titleBottom = r.top + render_detail::kSubGap + rowTitleH;
        drawText(rt, rowFmt.Get(), name, textBrush.Get(),
                 D2D1::RectF(tx, r.top + render_detail::kSubGap, r.right,
                             titleBottom));
        drawText(rt, subFmt.Get(), artist, subBrush.Get(),
                 D2D1::RectF(tx, titleBottom, r.right, r.bottom));
    }
    void card(const D2D1_RECT_F& r, bool square, const std::wstring& title,
              const std::wstring& sub, const std::string& coverUrl) const {
        const float w = r.right - r.left;
        const float coverH = square ? w : w * render_detail::kVideoAspect;
        drawCover(D2D1::RectF(r.left, r.top, r.right, r.top + coverH),
                  coverUrl);
        const float ty = r.top + coverH + render_detail::kCardInnerGap;
        const float titleBottom = ty + rowTitleH;
        drawText(rt, rowFmt.Get(), title, textBrush.Get(),
                 D2D1::RectF(r.left, ty, r.right, titleBottom));
        drawText(rt, subFmt.Get(), sub, subBrush.Get(),
                 D2D1::RectF(r.left, titleBottom, r.right, r.bottom));
    }
};

// Draws a song-list section: index + thumbnail + title + artist per row.
// titleFn / subFn / coverFn map each item to its display strings + cover URL.
template <class T, class TitleFn, class SubFn, class CoverFn>
void drawSongListSection(const Renderer& rn, const SectionLayout& sl,
                         const std::vector<T>& items, const std::wstring& label,
                         TitleFn titleFn, SubFn subFn, CoverFn coverFn) {
    if (!sl.present) {
        return;
    }
    rn.sectionTitle(sl, label);
    const size_t n = std::min(sl.items.size(), items.size());
    for (size_t i = 0; i < n; ++i) {
        const D2D1_RECT_F s = rn.screen(sl.items.at(i));
        if (!rn.visible(s)) {
            continue;
        }
        rn.songRow(s, static_cast<int>(i) + 1, utf8ToWide(titleFn(items.at(i))),
                   utf8ToWide(subFn(items.at(i))), coverFn(items.at(i)));
    }
}

// Draws a wrapped card grid: titleFn / subFn / coverFn map each item to its
// display strings + cover URL. `square` picks album/playlist (cover = card
// width) vs mv (16:9 cover).
template <class T, class TitleFn, class SubFn, class CoverFn>
void drawCardSection(const Renderer& rn, const SectionLayout& sl,
                     const std::vector<T>& items, const std::wstring& label,
                     bool square, TitleFn titleFn, SubFn subFn,
                     CoverFn coverFn) {
    if (!sl.present) {
        return;
    }
    rn.sectionTitle(sl, label);
    const size_t n = std::min(sl.items.size(), items.size());
    for (size_t i = 0; i < n; ++i) {
        const D2D1_RECT_F s = rn.screen(sl.items.at(i));
        if (!rn.visible(s)) {
            continue;
        }
        rn.card(s, square, utf8ToWide(titleFn(items.at(i))),
                utf8ToWide(subFn(items.at(i))), coverFn(items.at(i)));
    }
}

}  // namespace hemusic::ui
