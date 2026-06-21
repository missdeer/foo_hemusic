#include "ui/d2d.h"

#include <d2d1helper.h>

namespace hemusic::ui::d2d {

using Microsoft::WRL::ComPtr;

namespace {

// Picks the WIC pixel format D2D consumes directly: 32bpp BGRA premultiplied.
// Anything else would force an extra conversion pass at draw time.
// Not constexpr: WIC pixel-format GUIDs come from DEFINE_GUID
// (linker-resolved).
const GUID& kWicTargetFormat = GUID_WICPixelFormat32bppPBGRA;

}  // namespace

ID2D1Factory* factory() {
    static ComPtr<ID2D1Factory> g = [] {
        ComPtr<ID2D1Factory> f;
        // Single-threaded: all UI work happens on fb2k's main thread.
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          IID_PPV_ARGS(f.GetAddressOf()));
        return f;
    }();
    return g.Get();
}

IDWriteFactory* dwriteFactory() {
    static ComPtr<IDWriteFactory> g = [] {
        ComPtr<IDWriteFactory> f;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(f.GetAddressOf()));
        return f;
    }();
    return g.Get();
}

IWICImagingFactory* wicFactory() {
    // CoInitialize is the host's responsibility (fb2k's main thread is
    // already in an STA). If WIC creation fails we return null and callers
    // skip image decode.
    static ComPtr<IWICImagingFactory> g = [] {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(f.GetAddressOf()));
        return f;
    }();
    return g.Get();
}

HwndCanvas::HwndCanvas(HWND hwnd) : m_hwnd(hwnd) {}

bool HwndCanvas::ensureTarget() {
    if (m_target) {
        return true;
    }
    ID2D1Factory* f = factory();
    if (f == nullptr || m_hwnd == nullptr) {
        return false;
    }
    RECT rc{};
    if (GetClientRect(m_hwnd, &rc) == 0) {
        return false;
    }
    const UINT w = static_cast<UINT>(rc.right - rc.left);
    const UINT h = static_cast<UINT>(rc.bottom - rc.top);
    if (w == 0 || h == 0) {
        return false;
    }
    auto props = D2D1::RenderTargetProperties();
    auto hwndProps = D2D1::HwndRenderTargetProperties(
        m_hwnd, D2D1::SizeU(w, h), D2D1_PRESENT_OPTIONS_NONE);
    return SUCCEEDED(
        f->CreateHwndRenderTarget(props, hwndProps, m_target.GetAddressOf()));
}

void HwndCanvas::paint(
    const std::function<void(ID2D1HwndRenderTarget*)>& draw) {
    if (!ensureTarget()) {
        return;
    }
    m_target->BeginDraw();
    draw(m_target.Get());
    const HRESULT hr = m_target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // GPU reset / display mode change. Drop the dead target so the next
        // paint creates a fresh one, and ask for another paint right away.
        m_target.Reset();
        if (m_hwnd != nullptr) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void HwndCanvas::resize(UINT width, UINT height) {
    if (m_target && width != 0 && height != 0) {
        // Resize() is cheap when dimensions are unchanged; failure means the
        // target is doomed, drop it so the next paint recreates.
        if (FAILED(m_target->Resize(D2D1::SizeU(width, height)))) {
            m_target.Reset();
        }
    }
}

void HwndCanvas::discard() { m_target.Reset(); }

ComPtr<IWICBitmapSource> decodeImage(const void* data, size_t size) {
    IWICImagingFactory* wic = wicFactory();
    if (wic == nullptr || data == nullptr || size == 0) {
        return nullptr;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.GetAddressOf()))) {
        return nullptr;
    }
    // InitializeFromMemory takes a non-const pointer but does not write.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* bytes = const_cast<BYTE*>(static_cast<const BYTE*>(data));
    if (FAILED(stream->InitializeFromMemory(bytes, static_cast<DWORD>(size)))) {
        return nullptr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                            WICDecodeMetadataCacheOnLoad,
                                            decoder.GetAddressOf()))) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        return nullptr;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf()))) {
        return nullptr;
    }
    if (FAILED(converter->Initialize(frame.Get(), kWicTargetFormat,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return nullptr;
    }
    // The decoder pipeline above (stream -> decoder -> frame -> converter)
    // lazily reads from the input bytes via the IWICStream we created from
    // borrowed memory; if we returned `converter` directly, the caller's
    // bytes would have to outlive every later CopyPixels / D2D upload --
    // which they don't (ImageCache::onFetched destroys them on return).
    // CreateBitmap + CopyFromBitmapSource with WICBitmapCacheOnLoad copies
    // the pixels into a self-owned IWICBitmap up front so the result is
    // independent of `data` / the stream / the decoder chain.
    ComPtr<IWICBitmap> materialized;
    if (FAILED(wic->CreateBitmapFromSource(converter.Get(),
                                           WICBitmapCacheOnLoad,
                                           materialized.GetAddressOf()))) {
        return nullptr;
    }
    return materialized;
}

ComPtr<ID2D1Bitmap> makeBitmap(ID2D1RenderTarget* target,
                               IWICBitmapSource* source) {
    if (target == nullptr || source == nullptr) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(target->CreateBitmapFromWicBitmap(source, nullptr,
                                                 bitmap.GetAddressOf()))) {
        return nullptr;
    }
    return bitmap;
}

}  // namespace hemusic::ui::d2d
