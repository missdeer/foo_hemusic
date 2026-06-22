#include "ui/pages/section_render.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

#include "ui/d2d.h"
#include "ui/theme.h"

namespace hemusic::ui {

using Microsoft::WRL::ComPtr;

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

ComPtr<IDWriteTextFormat> makeFormat(const std::wstring& family, float size,
                                     bool centered, bool ellipsis) {
    ComPtr<IDWriteTextFormat> tf;
    IDWriteFactory* dw = d2d::dwriteFactory();
    if (dw == nullptr) {
        return tf;
    }
    dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         size, L"", tf.GetAddressOf());
    if (!tf) {
        return tf;
    }
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (centered) {
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (ellipsis) {
        // Ellipsize overlong titles so they can't spill across cells (the
        // no-clip scroll path relies on per-draw clipping, not a region clip).
        DWRITE_TRIMMING trim{};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(tf.Get(),
                                                     sign.GetAddressOf()))) {
            tf->SetTrimming(&trim, sign.Get());
        }
    }
    return tf;
}

void drawText(ID2D1RenderTarget* rt, IDWriteTextFormat* tf,
              const std::wstring& text, ID2D1Brush* brush,
              const D2D1_RECT_F& rect) {
    if (tf == nullptr || brush == nullptr || text.empty()) {
        return;
    }
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), tf, rect,
                  brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void drawCoverBitmap(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp,
                     const D2D1_RECT_F& dst) {
    const D2D1_SIZE_F bs = bmp->GetSize();
    const float tw = dst.right - dst.left;
    const float th = dst.bottom - dst.top;
    if (bs.width <= 0.0F || bs.height <= 0.0F || tw <= 0.0F || th <= 0.0F) {
        return;
    }
    const float targetAspect = tw / th;
    const float srcAspect = bs.width / bs.height;
    D2D1_RECT_F srcRect{};
    if (srcAspect > targetAspect) {  // source wider: crop left/right
        const float w = bs.height * targetAspect;
        const float x = (bs.width - w) * 0.5F;
        srcRect = D2D1::RectF(x, 0.0F, x + w, bs.height);
    } else {  // source taller: crop top/bottom
        const float h = bs.width / targetAspect;
        const float y = (bs.height - h) * 0.5F;
        srcRect = D2D1::RectF(0.0F, y, bs.width, y + h);
    }
    rt->DrawBitmap(bmp, dst, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                   &srcRect);
}

void drawCentered(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size,
                  const std::wstring& text) {
    ComPtr<IDWriteTextFormat> tf =
        makeFormat(theme.fontFamily, theme.rowTitleSize, /*centered=*/true,
                   /*ellipsis=*/false);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(theme.secondaryText,
                                         brush.GetAddressOf()))) {
        return;
    }
    drawText(rt, tf.Get(), text, brush.Get(),
             D2D1::RectF(0.0F, 0.0F, size.width, size.height));
}

float viewportHeightDip(HWND hwnd, float topInsetDip) {
    RECT rc{};
    if (hwnd == nullptr || GetClientRect(hwnd, &rc) == 0) {
        return 0.0F;
    }
    const float scale = d2d::dpiScaleForWindow(hwnd);
    const float h =
        static_cast<float>(rc.bottom - rc.top) / (scale > 0.0F ? scale : 1.0F);
    return h - topInsetDip > 0.0F ? h - topInsetDip : 0.0F;
}

}  // namespace hemusic::ui
