// foo_hemusic main UI panel (PLAN Phase 4/5: ui_element host).
// ui_element_v2 users drag into the Default UI layout. Owns the d2d canvas +
// theme snapshot and hosts a top tab bar (发现 / 搜索) switching between the
// DiscoverPage and the SearchPage. Both pages are windowless (they paint into
// this panel's render target); the only real child window is the search input
// box (a Win32 EDIT) shown above the search content. Colors / fonts follow the
// fb2k host theme (PLAN §3.5) and repaint on theme changes; the pages signal
// completion via their own PostMessage constants.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDK/foobar2000.h>
#include <SDK/ui_element.h>

#include <windows.h>

#include <commctrl.h>
#include <windowsx.h>

#include <d2d1.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "core/session.h"
#include "ui/d2d.h"
#include "ui/pages/discover_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/section_render.h"
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

// Posted to ourselves by the Session auth listener (which runs on the login
// worker thread) so the reload happens back on the UI thread. WM_APP+1/+3/+4/+5
// are taken by the pages' done / cover-ready messages, so this takes WM_APP+2.
constexpr UINT WM_HEMUSIC_AUTH_CHANGED = WM_APP + 2;

constexpr UINT_PTR kSearchEditId = 1;

// Top chrome heights (DIP). Tab bar always present; the search box band only
// applies on the search tab (the EDIT is hidden otherwise).
constexpr float kTabBarH = 36.0F;
constexpr float kSearchBoxH = 40.0F;
constexpr float kTabWidth = 84.0F;
constexpr float kTabPadding = 12.0F;      // left inset of the first tab
constexpr float kSearchBoxMargin = 4.0F;  // inset of the EDIT within its band
constexpr float kUnderlineH = 2.0F;       // active-tab underline thickness
constexpr float kUnderlineInset = 14.0F;  // underline horizontal inset per tab
constexpr float kRound = 0.5F;            // float->int rounding bias

enum class Tab : std::uint8_t { Discover = 0, Search = 1 };

constexpr int kTabCount = 2;

// DIP rect of tab `i` within the tab bar.
D2D1_RECT_F tabRectDip(int i) {
    const float left = kTabPadding + static_cast<float>(i) * kTabWidth;
    return D2D1::RectF(left, 0.0F, left + kTabWidth, kTabBarH);
}

}  // namespace

class MainPanel : public ui_element_instance {
   public:
    MainPanel(ui_element_config::ptr cfg, ui_element_instance_callback::ptr cb)
        : m_config(std::move(cfg)), m_callback(std::move(cb)) {}

    MainPanel(const MainPanel&) = delete;
    MainPanel(MainPanel&&) = delete;
    MainPanel& operator=(const MainPanel&) = delete;
    MainPanel& operator=(MainPanel&&) = delete;

    ~MainPanel() {
        unsubscribeAuth();
        if (m_hwnd != nullptr) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    void initializeWindow(HWND parent) {
        ensureWindowClass();
        m_hwnd = CreateWindowExW(
            0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
            0, 0, parent, nullptr, core_api::get_my_instance(), this);
        if (m_hwnd == nullptr) {
            return;
        }
        m_canvas = std::make_unique<d2d::HwndCanvas>(m_hwnd);
        m_discover.attach(m_hwnd);
        m_search.attach(m_hwnd);
        createSearchEdit();
        m_discover.load();
        // Re-pull when login/logout changes auth state. The callback fires on
        // the login worker thread, so it only marshals to the UI thread via
        // PostMessage; it captures the HWND (POD). Drop any prior subscription
        // first so a second initializeWindow can't leak the old listener.
        unsubscribeAuth();
        const HWND hwnd = m_hwnd;
        m_authListener = Session::instance().addAuthListener(
            [hwnd] { PostMessageW(hwnd, WM_HEMUSIC_AUTH_CHANGED, 0, 0); });
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
                    return 1;  // D2D paints the whole surface.
                case WM_PAINT:
                    self->onPaint();
                    return 0;
                case WM_SIZE:
                    if (self->m_canvas) {
                        self->m_canvas->resize(LOWORD(lp), HIWORD(lp));
                    }
                    self->positionSearchEdit();
                    return 0;
                case WM_LBUTTONDOWN:
                    self->onLeftDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                    return 0;
                case WM_MOUSEWHEEL:
                    self->onWheel(GET_WHEEL_DELTA_WPARAM(wp));
                    return 0;
                case DiscoverPage::kDoneMessage:
                case DiscoverPage::kCoverReadyMessage:
                    self->m_discover.onHostMessage(msg, wp, lp);
                    return 0;
                case SearchPage::kDoneMessage:
                case SearchPage::kCoverReadyMessage:
                    self->m_search.onHostMessage(msg, wp, lp);
                    return 0;
                case WM_HEMUSIC_AUTH_CHANGED:
                    self->m_discover.load();
                    self->m_search.reset();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case WM_DESTROY:
                    self->unsubscribeAuth();
                    if (self->m_searchFont != nullptr) {
                        DeleteObject(self->m_searchFont);
                        self->m_searchFont = nullptr;
                    }
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

    // Intercepts Enter inside the EDIT to submit the search (a single-line EDIT
    // would otherwise beep). Everything else falls through to the default proc.
    static LRESULT CALLBACK editSubclass(HWND hwnd, UINT msg, WPARAM wp,
                                         LPARAM lp, UINT_PTR id,
                                         DWORD_PTR ref) {
        auto* self = reinterpret_cast<MainPanel*>(ref);
        switch (msg) {
            case WM_GETDLGCODE:
                return DLGC_WANTALLKEYS;  // ensure Enter reaches us
            case WM_KEYDOWN:
                if (wp == VK_RETURN && self != nullptr) {
                    self->submitSearch();
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                RemoveWindowSubclass(hwnd, editSubclass, id);
                break;
            default:
                break;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    float contentTopDip() const {
        return m_tab == Tab::Search ? (kTabBarH + kSearchBoxH) : kTabBarH;
    }

    void createSearchEdit() {
        m_searchEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(kSearchEditId),
            core_api::get_my_instance(), nullptr);
        if (m_searchEdit == nullptr) {
            return;
        }
        SetWindowSubclass(m_searchEdit, editSubclass, kSearchEditId,
                          reinterpret_cast<DWORD_PTR>(this));
        EnableWindow(m_searchEdit, TRUE);
        SendMessageW(m_searchEdit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"搜索歌曲 / 歌手 / 专辑"));
        rebuildSearchFont();
        positionSearchEdit();
    }

    // (Re)creates the EDIT font for the window's current DPI + theme family.
    void rebuildSearchFont() {
        if (m_searchEdit == nullptr) {
            return;
        }
        const Theme theme = themeFromCallback(m_callback.get_ptr());
        const float scale = d2d::dpiScaleForWindow(m_hwnd);
        LOGFONTW lf{};  // zero-init leaves lfFaceName null-terminated
        lf.lfHeight = -static_cast<LONG>(theme.rowTitleSize * scale + kRound);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        const std::wstring& family = theme.fontFamily;
        const size_t n =
            std::min(family.size(), static_cast<size_t>(LF_FACESIZE - 1));
        std::copy_n(family.c_str(), n, std::begin(lf.lfFaceName));
        HFONT font = CreateFontIndirectW(&lf);
        if (font == nullptr) {
            return;
        }
        if (m_searchFont != nullptr) {
            DeleteObject(m_searchFont);
        }
        m_searchFont = font;
        SendMessageW(m_searchEdit, WM_SETFONT,
                     reinterpret_cast<WPARAM>(m_searchFont), TRUE);
    }

    void positionSearchEdit() {
        if (m_searchEdit == nullptr) {
            return;
        }
        RECT rc{};
        if (GetClientRect(m_hwnd, &rc) == 0) {
            return;
        }
        const float scale = d2d::dpiScaleForWindow(m_hwnd);
        const auto px = [scale](float dip) {
            return static_cast<int>(dip * scale + kRound);
        };
        const int x = px(kTabPadding);
        const int y = px(kTabBarH + kSearchBoxMargin);
        const int w = (rc.right - rc.left) - 2 * x;
        const int h = px(kSearchBoxH - 2.0F * kSearchBoxMargin);
        MoveWindow(m_searchEdit, x, y, std::max(0, w), std::max(0, h), TRUE);
    }

    void switchTab(Tab tab) {
        if (m_tab == tab) {
            return;
        }
        m_tab = tab;
        if (m_searchEdit != nullptr) {
            ShowWindow(m_searchEdit, tab == Tab::Search ? SW_SHOW : SW_HIDE);
            if (tab == Tab::Search) {
                SetFocus(m_searchEdit);
            }
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void onLeftDown(int xPx, int yPx) {
        const float scale = d2d::dpiScaleForWindow(m_hwnd);
        const float xDip = static_cast<float>(xPx) / (scale > 0 ? scale : 1.0F);
        const float yDip = static_cast<float>(yPx) / (scale > 0 ? scale : 1.0F);
        if (yDip > kTabBarH) {
            return;  // below the tab bar
        }
        for (int i = 0; i < kTabCount; ++i) {
            const D2D1_RECT_F r = tabRectDip(i);
            if (xDip >= r.left && xDip < r.right) {
                switchTab(static_cast<Tab>(i));
                return;
            }
        }
    }

    void onWheel(int delta) {
        if (m_tab == Tab::Discover) {
            m_discover.onMouseWheel(delta, kTabBarH);
        } else {
            m_search.onMouseWheel(delta, kTabBarH + kSearchBoxH);
        }
    }

    void submitSearch() {
        if (m_searchEdit == nullptr) {
            return;
        }
        const int len = GetWindowTextLengthW(m_searchEdit);
        // Buffer holds len chars + the null terminator GetWindowTextW writes.
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        const int copied = GetWindowTextW(m_searchEdit, text.data(), len + 1);
        text.resize(static_cast<size_t>(copied));
        m_search.search(text);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void drawTabBar(ID2D1RenderTarget* rt, const Theme& theme,
                    D2D1_SIZE_F size) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> tf =
            makeFormat(theme.fontFamily, theme.rowTitleSize, /*centered=*/true,
                       /*ellipsis=*/false);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textB;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> subB;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selB;
        if (FAILED(
                rt->CreateSolidColorBrush(theme.text, textB.GetAddressOf())) ||
            FAILED(rt->CreateSolidColorBrush(theme.secondaryText,
                                             subB.GetAddressOf())) ||
            FAILED(rt->CreateSolidColorBrush(theme.selection,
                                             selB.GetAddressOf()))) {
            return;
        }
        static const std::array<const wchar_t*, kTabCount> kLabels = {L"发现",
                                                                      L"搜索"};
        for (int i = 0; i < kTabCount; ++i) {
            const bool active = static_cast<int>(m_tab) == i;
            const D2D1_RECT_F r = tabRectDip(i);
            drawText(rt, tf.Get(), kLabels.at(i),
                     active ? textB.Get() : subB.Get(), r);
            if (active) {
                rt->FillRectangle(
                    D2D1::RectF(r.left + kUnderlineInset,
                                kTabBarH - kUnderlineH,
                                r.right - kUnderlineInset, kTabBarH),
                    selB.Get());
            }
        }
        // Thin separator under the whole bar.
        rt->FillRectangle(
            D2D1::RectF(0.0F, kTabBarH - 1.0F, size.width, kTabBarH),
            subB.Get());
    }

    void onPaint() {
        if (!m_canvas) {
            ValidateRect(m_hwnd, nullptr);
            return;
        }
        const Theme theme = themeFromCallback(m_callback.get_ptr());
        m_canvas->paint([&](ID2D1HwndRenderTarget* rt) {
            rt->Clear(theme.background);
            const D2D1_SIZE_F size = rt->GetSize();  // DIP
            drawTabBar(rt, theme, size);

            const float top = contentTopDip();
            const D2D1_SIZE_F pageSize =
                D2D1::SizeF(size.width, std::max(0.0F, size.height - top));
            rt->PushAxisAlignedClip(
                D2D1::RectF(0.0F, top, size.width, size.height),
                D2D1_ANTIALIAS_MODE_ALIASED);
            D2D1_MATRIX_3X2_F prev{};
            rt->GetTransform(&prev);
            rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0F, top) * prev);
            if (m_tab == Tab::Discover) {
                m_discover.paint(rt, theme, pageSize);
            } else {
                m_search.paint(rt, theme, pageSize);
            }
            rt->SetTransform(prev);
            rt->PopAxisAlignedClip();
        });
        ValidateRect(m_hwnd, nullptr);
    }

    void unsubscribeAuth() {
        if (m_authListener != 0) {
            Session::instance().removeAuthListener(m_authListener);
            m_authListener = 0;
        }
    }

    HWND m_hwnd = nullptr;
    HWND m_searchEdit = nullptr;
    HFONT m_searchFont = nullptr;
    Tab m_tab = Tab::Discover;
    std::unique_ptr<d2d::HwndCanvas> m_canvas;
    DiscoverPage m_discover;
    SearchPage m_search;
    Session::AuthListenerId m_authListener = 0;
    ui_element_config::ptr m_config;
    ui_element_instance_callback::ptr m_callback;
};

namespace {

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

    t_uint32 get_flags() override { return KFlagHavePopupCommand; }

    bool bump() override { return false; }
};

service_factory_single_t<MainElement> g_mainElementFactory;

}  // namespace

}  // namespace hemusic::ui
