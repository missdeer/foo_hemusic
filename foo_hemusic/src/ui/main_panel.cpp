// foo_hemusic main UI panel (PLAN.md Phase 4: ui_element host + discover_page).
// ui_element_v2 users drag into the Default UI layout editor. Owns the d2d
// canvas + theme snapshot and hosts the DiscoverPage, which loads /v1/platforms
// + /v1/page/discover on a worker thread and renders the new-songs list.
// Colors / fonts follow the fb2k host theme (PLAN §3.5) and repaint on theme
// changes; the worker signals completion via DiscoverPage::kDoneMessage.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDK/foobar2000.h>
#include <SDK/ui_element.h>

#include <windows.h>

#include <d2d1.h>

#include <memory>
#include <utility>

#include "ui/d2d.h"
#include "ui/pages/discover_page.h"
#include "ui/theme.h"

namespace hemusic::ui {

namespace {

// {7E2D6FB8-39A4-4D2E-B2C9-1B5E7F0B6E11}  -- unique per element, do not reuse.
constexpr GUID kGuidMainPanel = {
    0x7e2d6fb8,
    0x39a4,
    0x4d2e,
    {0xb2, 0xc9, 0x1b, 0x5e, 0x7f, 0x0b, 0x6e, 0x11}};

constexpr wchar_t kWindowClass[] = L"foo_hemusic_main_panel";

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
            m_discover.attach(m_hwnd);
            m_discover.load();
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
                case DiscoverPage::kDoneMessage:
                    self->m_discover.onHostMessage(msg, wp, lp);
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
        const Theme theme = themeFromCallback(m_callback.get_ptr());
        m_canvas->paint([&](ID2D1HwndRenderTarget* rt) {
            rt->Clear(theme.background);
            m_discover.paint(rt, theme, rt->GetSize());
        });
        ValidateRect(m_hwnd, nullptr);
    }

    HWND m_hwnd = nullptr;
    std::unique_ptr<d2d::HwndCanvas> m_canvas;
    DiscoverPage m_discover;
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
