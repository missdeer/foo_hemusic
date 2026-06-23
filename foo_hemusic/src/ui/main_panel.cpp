// foo_hemusic main UI panel (PLAN Phase 4/5: ui_element host).
// ui_element_v2 users drag into the Default UI layout. Owns the d2d canvas +
// theme snapshot and hosts a top tab bar (发现 / 搜索) plus a nav::Stack that
// the detail pages (HEMUSIC-15+) push/pop onto. Discover / Search / Playlist
// detail pages are all windowless (they paint into this panel's render
// target); the only real child window is the search input box (a Win32 EDIT)
// shown above the search content when the search root is active. Colors /
// fonts follow the fb2k host theme (PLAN §3.5) and repaint on theme changes;
// the pages signal completion via their own PostMessage constants.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDK/foobar2000.h>
#include <SDK/popup_message.h>
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
#include "ui/nav.h"
#include "ui/pages/album_detail_page.h"
#include "ui/pages/discover_page.h"
#include "ui/pages/playlist_detail_page.h"
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
// applies on the search root (the EDIT is hidden otherwise). kTabPadding is
// widened to 56 DIP (★ S1) so the back-button slot [kBackBtnLeft,
// kBackBtnRight] sits in reserved space without shifting tab positions when
// canGoBack flips.
constexpr float kTabBarH = 36.0F;
constexpr float kSearchBoxH = 40.0F;
constexpr float kTabWidth = 84.0F;
constexpr float kTabPadding = 56.0F;  // left inset of the first tab
constexpr float kBackBtnLeft = 8.0F;
constexpr float kBackBtnRight = 48.0F;
constexpr float kSearchBoxMargin = 4.0F;  // inset of the EDIT within its band
constexpr float kUnderlineH = 2.0F;       // active-tab underline thickness
constexpr float kUnderlineInset = 14.0F;  // underline horizontal inset per tab
constexpr float kRound = 0.5F;            // float->int rounding bias

enum class Tab : std::uint8_t { Discover = 0, Search = 1 };

constexpr int kTabCount = 2;

// Which kind of click capture the main panel is currently routing.
enum class CaptureOwner : std::uint8_t { None, BackBtn, Page };

nav::PageKind tabToKind(Tab t) {
    return t == Tab::Search ? nav::PageKind::Search : nav::PageKind::Discover;
}

const char* tabLabel(Tab t) { return t == Tab::Search ? "搜索" : "发现"; }

// DIP rect of tab `i` within the tab bar.
D2D1_RECT_F tabRectDip(int i) {
    const float left = kTabPadding + static_cast<float>(i) * kTabWidth;
    return D2D1::RectF(left, 0.0F, left + kTabWidth, kTabBarH);
}

D2D1_RECT_F backBtnRectDip() {
    return D2D1::RectF(kBackBtnLeft, 0.0F, kBackBtnRight, kTabBarH);
}

bool rectHit(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
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
        m_playlistDetail.attach(m_hwnd);
        m_albumDetail.attach(m_hwnd);
        m_stack.replaceRoot({nav::PageKind::Discover, "发现", {}});
        m_discover.setOnPlaylistOpen(
            [this](const PlaylistInfo& p) { openPlaylistDetail(p); });
        m_discover.setOnAlbumOpen(
            [this](const AlbumInfo& a) { openAlbumDetail(a); });
        m_playlistDetail.setOnEnqueueAll([](const PlaylistInfo& p,
                                            const std::vector<SongInfo>& s) {
            // ★ M1: SDK helper -- popup_message::g_show, NOT
            // popup_message_v3::show (that signature does not exist).
            // Default-construct then append to dodge the SDK's stringLite
            // virtual-call-in-ctor analyzer warning.
            pfc::string8 msg;
            msg =
                "全部入列功能尚未实装(HEMUSIC-5: playlist_writer)\n当前歌单：";
            msg += p.name.c_str();
            msg += " (";
            msg += pfc::format_int(static_cast<int64_t>(s.size()));
            msg += " 首)";
            popup_message::g_show(msg, "HE-Music");
        });
        m_albumDetail.setOnEnqueueAll([](const AlbumInfo& a) {
            pfc::string8 msg;
            msg =
                "全部入列功能尚未实装(HEMUSIC-5: playlist_writer)\n当前专辑：";
            msg += a.name.c_str();
            msg += " (";
            msg += pfc::format_int(static_cast<int64_t>(a.songs.size()));
            msg += " 首)";
            popup_message::g_show(msg, "HE-Music");
        });
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
                case WM_LBUTTONUP:
                    self->onLeftUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                    return 0;
                case WM_MOUSEMOVE:
                    self->onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                    return 0;
                case WM_MOUSELEAVE:
                    self->onMouseLeave();
                    return 0;
                case WM_CAPTURECHANGED:
                    self->onCaptureChanged(reinterpret_cast<HWND>(lp));
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
                case PlaylistDetailPage::kDoneMessage:
                case PlaylistDetailPage::kCoverReadyMessage:
                    self->m_playlistDetail.onHostMessage(msg, wp, lp);
                    return 0;
                case AlbumDetailPage::kDoneMessage:
                case AlbumDetailPage::kCoverReadyMessage:
                    self->m_albumDetail.onHostMessage(msg, wp, lp);
                    return 0;
                case WM_HEMUSIC_AUTH_CHANGED:
                    // ★★ S7: discover/search workers may still in-flight after
                    // logout (known, not in HEMUSIC-15 scope); detail pages are
                    // bumped via reset() to drop their workers' writes.
                    self->m_discover.load();
                    self->m_search.reset();
                    self->m_playlistDetail.reset();
                    self->m_albumDetail.reset();
                    self->m_stack.replaceRoot(
                        {tabToKind(self->m_tab), tabLabel(self->m_tab), {}});
                    self->updateSearchEditVisibility();
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

    nav::PageKind currentKind() const {
        return m_stack.empty() ? tabToKind(m_tab) : m_stack.current().kind;
    }

    // The search EDIT band only shows when the search root is current; detail
    // pages pushed onto the stack (PlaylistDetail etc.) hide it even when the
    // user's last selected tab was Search.
    float contentTopDip() const {
        return currentKind() == nav::PageKind::Search ? (kTabBarH + kSearchBoxH)
                                                      : kTabBarH;
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
        // ★ S3: if the user clicks the active tab while a detail page is
        // pushed, pop back to the tab's root (replaceRoot clears the trail).
        const bool sameTab = (m_tab == tab);
        if (sameTab && !m_stack.canGoBack()) {
            return;
        }
        m_tab = tab;
        m_stack.replaceRoot({tabToKind(tab), tabLabel(tab), {}});
        // ★★ S7: bump detail pages' generation so any in-flight worker drops
        // its result instead of writing into stale state visible after the pop.
        m_playlistDetail.reset();
        m_albumDetail.reset();
        updateSearchEditVisibility();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void updateSearchEditVisibility() {
        if (m_searchEdit == nullptr) {
            return;
        }
        const bool show = (currentKind() == nav::PageKind::Search);
        ShowWindow(m_searchEdit, show ? SW_SHOW : SW_HIDE);
        if (show) {
            SetFocus(m_searchEdit);
        }
    }

    void openPlaylistDetail(const PlaylistInfo& p) {
        nav::PageEntry e;
        e.kind = nav::PageKind::PlaylistDetail;
        e.title = p.name;
        e.params["id"] = p.id;
        e.params["platform"] = p.platform;
        e.params["title"] = p.name;
        m_stack.push(e);
        m_playlistDetail.enter(e);
        updateSearchEditVisibility();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void openAlbumDetail(const AlbumInfo& a) {
        nav::PageEntry e;
        e.kind = nav::PageKind::AlbumDetail;
        e.title = a.name;
        e.params["id"] = a.id;
        e.params["platform"] = a.platform;
        e.params["title"] = a.name;
        m_stack.push(e);
        m_albumDetail.enter(e);
        updateSearchEditVisibility();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void popPage() {
        if (!m_stack.canGoBack()) {
            return;
        }
        m_stack.pop();
        // ★★ S7: stop the (now-orphaned) detail worker from clobbering the
        // page state the user just navigated back to.
        m_playlistDetail.reset();
        m_albumDetail.reset();
        updateSearchEditVisibility();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Converts a physical mouse point to DIP. Returns false for a degenerate
    // scale (defensive; dpiScaleForWindow falls back to 96 when unknown).
    void pxToDip(int xPx, int yPx, float& xDip, float& yDip) const {
        const float scale = d2d::dpiScaleForWindow(m_hwnd);
        const float s = scale > 0 ? scale : 1.0F;
        xDip = static_cast<float>(xPx) / s;
        yDip = static_cast<float>(yPx) / s;
    }

    void ensureMouseTracking() {
        if (m_trackingMouse) {
            return;
        }
        TRACKMOUSEEVENT t{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hwnd, 0};
        if (TrackMouseEvent(&t) != 0) {
            m_trackingMouse = true;
        }
    }

    void onLeftDown(int xPx, int yPx) {
        float xDip = 0;
        float yDip = 0;
        pxToDip(xPx, yPx, xDip, yDip);
        // Chrome (back button + tab bar) lives in [0, kTabBarH).
        if (yDip < kTabBarH) {
            if (m_stack.canGoBack() && rectHit(backBtnRectDip(), xDip, yDip)) {
                m_backBtnPressing = true;
                m_backBtnHover = true;
                SetCapture(m_hwnd);
                m_capture = CaptureOwner::BackBtn;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            for (int i = 0; i < kTabCount; ++i) {
                const D2D1_RECT_F r = tabRectDip(i);
                if (xDip >= r.left && xDip < r.right) {
                    switchTab(static_cast<Tab>(i));
                    return;
                }
            }
            return;
        }
        const float top = contentTopDip();
        if (yDip < top) {
            return;  // inside the search-box band (Win32 EDIT owns input)
        }
        // Forward to the current page in page-content DIP coordinates.
        if (currentKind() == nav::PageKind::Discover) {
            m_discover.onLeftDown(xDip, yDip - top);
        } else if (currentKind() == nav::PageKind::PlaylistDetail) {
            if (m_playlistDetail.onLeftDown(xDip, yDip - top)) {
                SetCapture(m_hwnd);
                m_capture = CaptureOwner::Page;
            }
        } else if (currentKind() == nav::PageKind::AlbumDetail) {
            if (m_albumDetail.onLeftDown(xDip, yDip - top)) {
                SetCapture(m_hwnd);
                m_capture = CaptureOwner::Page;
            }
        }
        // SearchPage clicks are handled by the EDIT subclass; nothing to do.
    }

    void onLeftUp(int xPx, int yPx) {
        float xDip = 0;
        float yDip = 0;
        pxToDip(xPx, yPx, xDip, yDip);
        const CaptureOwner owner = m_capture;
        // ★ R1-M1: dispatch the click outcome BEFORE ReleaseCapture.
        // ReleaseCapture synchronously fires WM_CAPTURECHANGED ->
        // onCaptureLost(), which clears the page's btnPressing_ / our
        // m_backBtnPressing; if we release first, the subsequent dispatch
        // sees pressing == false and never fires the callback.
        m_capture = CaptureOwner::None;
        if (owner == CaptureOwner::BackBtn) {
            const bool over = rectHit(backBtnRectDip(), xDip, yDip);
            m_backBtnPressing = false;
            m_backBtnHover = over;
            ReleaseCapture();  // safe: state already consumed
            if (over) {
                popPage();
            } else {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        if (owner == CaptureOwner::Page) {
            const float top = contentTopDip();
            if (currentKind() == nav::PageKind::AlbumDetail) {
                m_albumDetail.onLeftUp(xDip, yDip - top);
            } else {
                m_playlistDetail.onLeftUp(xDip, yDip - top);
            }
            ReleaseCapture();  // page already consumed its pressing flag
        }
    }

    void onMouseMove(int xPx, int yPx) {
        ensureMouseTracking();
        float xDip = 0;
        float yDip = 0;
        pxToDip(xPx, yPx, xDip, yDip);
        // Back button hover follows the cursor regardless of capture state.
        const bool overBack =
            m_stack.canGoBack() && rectHit(backBtnRectDip(), xDip, yDip);
        if (overBack != m_backBtnHover) {
            m_backBtnHover = overBack;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        if (m_capture == CaptureOwner::Page ||
            (m_capture == CaptureOwner::None &&
             (currentKind() == nav::PageKind::PlaylistDetail ||
              currentKind() == nav::PageKind::AlbumDetail))) {
            const float top = contentTopDip();
            const float pageY = yDip - top;
            // Negative pageY (in chrome) still gets forwarded so the page can
            // clear its own hover state when the cursor drifts up.
            if (currentKind() == nav::PageKind::AlbumDetail) {
                m_albumDetail.onMouseMove(xDip, pageY);
            } else {
                m_playlistDetail.onMouseMove(xDip, pageY);
            }
        }
    }

    void onMouseLeave() {
        m_trackingMouse = false;
        if (m_backBtnHover) {
            m_backBtnHover = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        if (currentKind() == nav::PageKind::PlaylistDetail) {
            m_playlistDetail.onMouseLeave();
        } else if (currentKind() == nav::PageKind::AlbumDetail) {
            m_albumDetail.onMouseLeave();
        }
    }

    void onCaptureChanged(HWND newCaptureHwnd) {
        if (newCaptureHwnd == m_hwnd) {
            return;  // we re-took capture ourselves; nothing to do
        }
        const CaptureOwner owner = m_capture;
        m_capture = CaptureOwner::None;
        if (owner == CaptureOwner::BackBtn) {
            m_backBtnPressing = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        } else if (owner == CaptureOwner::Page) {
            if (currentKind() == nav::PageKind::AlbumDetail) {
                m_albumDetail.onCaptureLost();
            } else {
                m_playlistDetail.onCaptureLost();
            }
        }
    }

    void onWheel(int delta) {
        switch (currentKind()) {
            case nav::PageKind::Discover:
                m_discover.onMouseWheel(delta, kTabBarH);
                break;
            case nav::PageKind::Search:
                m_search.onMouseWheel(delta, kTabBarH + kSearchBoxH);
                break;
            case nav::PageKind::PlaylistDetail:
                m_playlistDetail.onMouseWheel(delta, kTabBarH);
                break;
            case nav::PageKind::AlbumDetail:
                m_albumDetail.onMouseWheel(delta, kTabBarH);
                break;
            default:
                break;
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

    void drawBackButton(ID2D1RenderTarget* rt, IDWriteTextFormat* tf,
                        ID2D1SolidColorBrush* textB,
                        ID2D1SolidColorBrush* subB) {
        if (!m_stack.canGoBack()) {
            return;
        }
        const D2D1_RECT_F r = backBtnRectDip();
        // Subtle hover fill (secondary text alpha-tinted as a cheap surface).
        if (m_backBtnHover) {
            const D2D1_COLOR_F bg{0.5F, 0.5F, 0.5F,
                                  m_backBtnPressing ? 0.35F : 0.18F};
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverB;
            if (SUCCEEDED(
                    rt->CreateSolidColorBrush(bg, hoverB.GetAddressOf()))) {
                rt->FillRectangle(r, hoverB.Get());
            }
        }
        drawText(rt, tf, L"‹ 返回", m_backBtnHover ? textB : subB, r);
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
        drawBackButton(rt, tf.Get(), textB.Get(), subB.Get());

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
            switch (currentKind()) {
                case nav::PageKind::Discover:
                    m_discover.paint(rt, theme, pageSize);
                    break;
                case nav::PageKind::Search:
                    m_search.paint(rt, theme, pageSize);
                    break;
                case nav::PageKind::PlaylistDetail:
                    m_playlistDetail.paint(rt, theme, pageSize);
                    break;
                case nav::PageKind::AlbumDetail:
                    m_albumDetail.paint(rt, theme, pageSize);
                    break;
                default:
                    break;
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
    PlaylistDetailPage m_playlistDetail;
    AlbumDetailPage m_albumDetail;
    nav::Stack m_stack;
    // Capture lifecycle (★ M5): MainPanel SetCapture's its own HWND when the
    // back button or current page reports a button-press, so subsequent
    // WM_MOUSEMOVE / LBUTTONUP / CAPTURECHANGED route to us.
    CaptureOwner m_capture = CaptureOwner::None;
    bool m_trackingMouse = false;  // TrackMouseEvent(TME_LEAVE) armed?
    bool m_backBtnHover = false;
    bool m_backBtnPressing = false;
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
