#pragma once

// Direct2D / DirectWrite / WIC infrastructure shared by every custom-drawn
// widget in foo_hemusic (PLAN.md Phase 4: ui/d2d.{h,cpp}).
//
// Three process-wide factories created on first access (magic-statics, so
// thread-safe init), plus a per-HWND canvas that owns the render target +
// handles device loss and resize.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <functional>

namespace hemusic::ui::d2d {

// Factory accessors. Each is lazily created, single-threaded mode (all UI
// runs on fb2k's main thread). Returns nullptr if creation failed -- callers
// should treat that as fatal and skip painting.
ID2D1Factory* factory();
IDWriteFactory* dwriteFactory();
IWICImagingFactory* wicFactory();

// Render target bound to a single HWND. Created lazily inside paint() so the
// HWND already has its client size. Releases the target on WM_DESTROY (via
// discard()) and on D2DERR_RECREATE_TARGET (transparently inside paint()).
class HwndCanvas {
   public:
    explicit HwndCanvas(HWND hwnd);
    HwndCanvas(const HwndCanvas&) = delete;
    HwndCanvas(HwndCanvas&&) = delete;
    HwndCanvas& operator=(const HwndCanvas&) = delete;
    HwndCanvas& operator=(HwndCanvas&&) = delete;
    ~HwndCanvas() = default;

    // Drives BeginDraw / EndDraw. If the target was lost, drops it and
    // invalidates the window so WM_PAINT retries with a fresh target. No-op
    // when the factory is missing or the HWND has zero client area.
    void paint(const std::function<void(ID2D1HwndRenderTarget*)>& draw);

    // Hooked from WM_SIZE. Cheap if the target hasn't been created yet.
    void resize(UINT width, UINT height);

    // Hooked from WM_DESTROY. Releases the target without touching the HWND.
    void discard();

   private:
    bool ensureTarget();

    HWND m_hwnd;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_target;
};

// Decode an encoded image (PNG/JPG/etc.) into a 32bpp BGRA premultiplied
// frame ready to upload into a D2D bitmap. Returns null on failure.
Microsoft::WRL::ComPtr<IWICBitmapSource> decodeImage(const void* data,
                                                     size_t size);

// Upload a decoded WIC source to a D2D bitmap on the given render target.
// The bitmap is a device-dependent resource: drop it when the target is
// recreated. Returns null on failure.
Microsoft::WRL::ComPtr<ID2D1Bitmap> makeBitmap(ID2D1RenderTarget* target,
                                               IWICBitmapSource* source);

}  // namespace hemusic::ui::d2d
