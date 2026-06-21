// foo_hemusic main UI panel (PLAN.md Phase 4 step 1 + d2d integration).
// Minimal ui_element_v2 that registers an empty panel users can drag into the
// Default UI layout editor. Paints a centered "HE-Music" placeholder via
// Direct2D + DirectWrite, taking the background / text color from the fb2k
// host theme. Doubles as the smoke test for the d2d module: render-target
// lifecycle, device-loss handling and theme repaints all run through here
// before any widgets ride on top.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDK/foobar2000.h>
#include <SDK/ui_element.h>

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <memory>
#include <utility>

#include "ui/d2d.h"

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;

// {7E2D6FB8-39A4-4D2E-B2C9-1B5E7F0B6E11}  -- unique per element, do not reuse.
constexpr GUID kGuidMainPanel = {
    0x7e2d6fb8,
    0x39a4,
    0x4d2e,
    {0xb2, 0xc9, 0x1b, 0x5e, 0x7f, 0x0b, 0x6e, 0x11}};

constexpr wchar_t kWindowClass[] = L"foo_hemusic_main_panel";
constexpr wchar_t kPlaceholderText[] = L"HE-Music";
constexpr wchar_t kPlaceholderFont[] = L"Segoe UI";
constexpr float kPlaceholderFontSize = 18.0F;
constexpr float kColorScale = 255.0F;

D2D1_COLOR_F toColorF(COLORREF c) {
    return D2D1::ColorF(static_cast<float>(GetRValue(c)) / kColorScale,
                        static_cast<float>(GetGValue(c)) / kColorScale,
                        static_cast<float>(GetBValue(c)) / kColorScale, 1.0F);
}

ComPtr<IDWriteTextFormat> placeholderTextFormat() {
    static ComPtr<IDWriteTextFormat> g = [] {
        ComPtr<IDWriteTextFormat> tf;
        IDWriteFactory* dw = d2d::dwriteFactory();
        if (dw == nullptr) {
            return tf;
        }
        dw->CreateTextFormat(
            kPlaceholderFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            kPlaceholderFontSize, L"", tf.GetAddressOf());
        if (tf) {
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return tf;
    }();
    return g;
}

class MainPanel : public ui_element_instance {
   public:
    MainPanel(ui_element_config::ptr cfg, ui_element_instance_callback::ptr cb)
        : m_config(std::move(cfg)), m_callback(std::move(cb)) {}

    MainPanel(const MainPanel&) = delete;
    MainPanel(MainPanel&&) = delete;
    MainPanel& operator=(const MainPanel&) = delete;
    MainPanel& operator=(MainPanel&&) = delete;

    ~MainPanel() {
        if (m_hwnd != nullptr) {
            // Host released us without destroying the HWND first -- SDK
            // contract says we own teardown then.
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    void initializeWindow(HWND parent) {
        ensureWindowClass();
        m_hwnd = CreateWindowExW(
            0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
            0, 0, parent, nullptr, core_api::get_my_instance(), this);
        if (m_hwnd != nullptr) {
            m_canvas = std::make_unique<d2d::HwndCanvas>(m_hwnd);
        }
    }

    fb2k::hwnd_t get_wnd() override { return m_hwnd; }
    void set_configuration(ui_element_config::ptr cfg) override {
        m_config = std::move(cfg);
    }
    ui_element_config::ptr get_configuration() override { return m_config; }
    GUID get_guid() override { return kGuidMainPanel; }
    GUID get_subclass() override { return ui_element_subclass_utility; }

    void notify(const GUID& what, t_size /*p1*/, const void* /*p2*/,
                t_size /*p2size*/) override {
        if ((what == ui_element_notify_colors_changed ||
             what == ui_element_notify_font_changed) &&
            m_hwnd != nullptr) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

   private:
    static void ensureWindowClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // No hbrBackground / no GDI WM_ERASEBKGND fill -- D2D owns the whole
        // client area so an extra GDI fill would just cause flicker.
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<MainPanel*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            switch (msg) {
                case WM_ERASEBKGND:
                    // D2D paints the whole surface; suppress GDI erase.
                    return 1;
                case WM_PAINT:
                    self->onPaint();
                    return 0;
                case WM_SIZE:
                    if (self->m_canvas) {
                        self->m_canvas->resize(LOWORD(lp), HIWORD(lp));
                    }
                    return 0;
                case WM_DESTROY:
                    if (self->m_canvas) {
                        self->m_canvas->discard();
                    }
                    self->m_hwnd = nullptr;
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    break;
                default:
                    break;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void onPaint() {
        if (!m_canvas) {
            ValidateRect(m_hwnd, nullptr);
            return;
        }
        const D2D1_COLOR_F bg =
            toColorF(m_callback->query_std_color(ui_color_background));
        const D2D1_COLOR_F fg =
            toColorF(m_callback->query_std_color(ui_color_text));
        m_canvas->paint([&](ID2D1HwndRenderTarget* rt) {
            rt->Clear(bg);
            ComPtr<IDWriteTextFormat> tf = placeholderTextFormat();
            if (!tf) {
                return;
            }
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(rt->CreateSolidColorBrush(fg, brush.GetAddressOf()))) {
                return;
            }
            const D2D1_SIZE_F size = rt->GetSize();
            const auto rect = D2D1::RectF(0.0F, 0.0F, size.width, size.height);
            rt->DrawTextW(kPlaceholderText,
                          static_cast<UINT32>(std::wcslen(kPlaceholderText)),
                          tf.Get(), rect, brush.Get());
        });
        ValidateRect(m_hwnd, nullptr);
    }

    HWND m_hwnd = nullptr;
    std::unique_ptr<d2d::HwndCanvas> m_canvas;
    ui_element_config::ptr m_config;
    ui_element_instance_callback::ptr m_callback;
};

class MainElement : public ui_element_v2 {
   public:
    GUID get_guid() override { return kGuidMainPanel; }
    GUID get_subclass() override { return ui_element_subclass_utility; }
    void get_name(pfc::string_base& out) override { out = "HE-Music"; }

    ui_element_instance::ptr instantiate(
        fb2k::hwnd_t parent, ui_element_config::ptr cfg,
        ui_element_instance_callback::ptr cb) override {
        PFC_ASSERT(cfg->get_guid() == get_guid());
        auto inst = fb2k::service_new<MainPanel>(cfg, cb);
        inst->initializeWindow(parent);
        return inst;
    }

    ui_element_config::ptr get_default_configuration() override {
        return ui_element_config::g_create_empty(get_guid());
    }

    ui_element_children_enumerator_ptr enumerate_children(
        ui_element_config::ptr /*cfg*/) override {
        return nullptr;
    }

    bool get_description(pfc::string_base& out) override {
        out = "HE-Music online music source panel.";
        return true;
    }

    t_uint32 get_flags() override {
        // Auto-generate a popup menu command (View > HE-Music) so users can
        // open the panel without manually editing the layout. Bump isn't
        // supported yet -- single popup window is fine for the placeholder.
        return KFlagHavePopupCommand;
    }

    bool bump() override { return false; }
};

service_factory_single_t<MainElement> g_mainElementFactory;

}  // namespace

}  // namespace hemusic::ui
