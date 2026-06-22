#include "ui/pages/login_dlg.h"

// The SDK (pfc-lite.h) includes WinSock2.h before windows.h; include it before
// any of our own Win32 headers so the legacy winsock.h never sneaks in first
// (mixing the two gives redefinition errors).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDK/foobar2000.h>
#include <SDK/menu.h>

#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <memory>
#include <string>
#include <thread>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "api/user.h"
#include "auth/device_info.h"
#include "auth/login_flow.h"
#include "auth/token_store.h"
#include "core/config.h"
#include "core/session.h"
#include "net/http_client.h"
#include "ui/d2d.h"
#include "ui/theme.h"

// Threading model: the menu command runs on fb2k's main thread and creates this
// modeless window there, so fb2k's message pump dispatches its messages. The
// blocking login (WinHTTP) runs on a worker std::thread; it talks back to the
// UI only via PostMessage (fire-and-forget) -- never a synchronous SendMessage
// -- so the UI thread can join the worker without risking a cross-thread
// deadlock:
//   - PostMessage WM_LOGIN_PROGRESS  -> update phase, repaint
//   - PostMessage WM_LOGIN_DONE      -> report result + tear down
// openUrl (ShellExecuteW) runs on the worker itself (COM-initialized). Teardown
// always sets the cancel event before joining, so a worker mid-poll exits
// promptly instead of hanging the join. Cancellation is a manual-reset event
// the WaitFn polls between status polls.
//
// Presentation (Phase 5 / HEMUSIC-12): the dialog draws itself with Direct2D so
// it fuses with the fb2k host theme (colors + font via themeFromHost), shows an
// animated progress ring while waiting for authorization, and a custom-drawn
// cancel button. The worker/threading layer above is unchanged.

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kAppVersion = "0.0.1";  // mirrors component.cpp
constexpr wchar_t kWindowClass[] = L"foo_hemusic_login";

constexpr UINT WM_LOGIN_PROGRESS = WM_APP + 1;
constexpr UINT WM_LOGIN_DONE = WM_APP + 2;

constexpr UINT_PTR kRingTimerId = 1;
constexpr UINT kRingTimerMs = 33;        // ~30 fps spinner
constexpr float kRingDegPerTick = 9.0F;  // rotation step per timer tick

constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;          // exclusive (2xx)
constexpr DWORD kMillisPerSecond = 1000;  // WaitForSingleObject unit
constexpr INT_PTR kShellExecOk = 32;      // ShellExecuteW success is > 32

// Layout in device-independent pixels (the render target uses desktop DPI, so
// rt->GetSize() returns these DIPs regardless of the physical pixel size we
// create the window at). Hit-testing converts mouse pixels back to DIPs via the
// captured DPI scale.
constexpr float kDlgW = 360.0F;
constexpr float kDlgH = 212.0F;
constexpr float kTitleY = 18.0F;
constexpr float kTitleSize = 17.0F;
constexpr float kRingCenterY = 78.0F;
constexpr float kRingRadius = 22.0F;
constexpr float kRingStroke = 3.5F;
constexpr float kTrackStroke = 2.0F;
constexpr float kRingSweepDeg = 270.0F;
constexpr float kStatusY = 116.0F;
constexpr float kStatusH = 24.0F;
constexpr float kStatusSize = 14.0F;
constexpr float kBtnW = 96.0F;
constexpr float kBtnH = 32.0F;
constexpr float kBtnY = 156.0F;
constexpr float kBtnRadius = 5.0F;
constexpr float kBtnTextSize = 14.0F;

constexpr float kBaseDpi = 96.0F;
constexpr float kDeg2Rad = 3.14159265F / 180.0F;
constexpr float kFullCircleDeg = 360.0F;
constexpr float kHalf = 0.5F;
constexpr float kTitleBottomPad = 4.0F;
// How far to push the button fill toward text on press, for click feedback.
constexpr float kPressMix = 0.2F;

// {BCE470A9-E0B2-4130-8B39-85840CB44BA8}
constexpr GUID kGuidMenuGroup = {
    0xbce470a9,
    0xe0b2,
    0x4130,
    {0x8b, 0x39, 0x85, 0x84, 0x0c, 0xb4, 0x4b, 0xa8}};
// {E14AD12E-5845-4BD2-BD20-C3407AEBAAA8}
constexpr GUID kGuidCmdLogin = {
    0xe14ad12e,
    0x5845,
    0x4bd2,
    {0xbd, 0x20, 0xc3, 0x40, 0x7a, 0xeb, 0xaa, 0xa8}};

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

boost::json::value parseJson(const std::string& s) {
    boost::system::error_code ec;
    auto v = boost::json::parse(s, ec);
    return ec ? boost::json::value(nullptr) : v;
}

const wchar_t* phaseText(LoginPhase phase) {
    switch (phase) {
        case LoginPhase::Connecting:
            return L"正在连接 HE-Music…";
        case LoginPhase::WaitingForAuthorization:
            return L"请在浏览器中完成 Linux.do 授权…";
        case LoginPhase::Finalizing:
            return L"授权成功，正在获取账号…";
    }
    return L"";
}

D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, 1.0F);
}

// Login result, filled by the worker just before it posts WM_LOGIN_DONE and
// read by the handler. Embedded in LoginUi (not heap-passed via lParam) so the
// normal teardown frees it -- a posted-but-purged message during external
// window destruction can't leak it.
struct DoneResult {
    LoginOutcome outcome = LoginOutcome::TransportError;
    std::string detail;     // username on success, else error message
    bool persisted = true;  // whether the token reached disk (success path)
};

// State for one open dialog; owned by the window (GWLP_USERDATA), freed in
// WM_DESTROY after the worker has joined. Single instance via g_active.
struct LoginUi {
    HWND hwnd = nullptr;
    HANDLE cancelEvent = nullptr;  // manual-reset
    std::thread worker;
    std::unique_ptr<d2d::HwndCanvas> canvas;

    // UI-thread-only render state (the worker only mutates via PostMessage).
    LoginPhase phase = LoginPhase::Connecting;
    float ringAngle = 0.0F;
    bool cancelling = false;
    bool btnHover = false;
    bool btnPressed = false;
    bool tracking = false;  // WM_MOUSELEAVE armed
    float dpiScale = 1.0F;  // physical pixels per DIP

    // Written once by the worker before its WM_LOGIN_DONE post; read by the
    // handler after. No overlap, so the post's ordering suffices (no lock).
    DoneResult done;

    // Captured on the main thread so the worker never touches cfg_var/core_api.
    // Disk persistence on success goes through Session, which already holds
    // the path captured at component init.
    std::string baseUrl;
    DeviceInfo device;
};

LoginUi* g_active = nullptr;  // main-thread only

// GET /v1/user/info with the fresh token -> human display name. Prefers
// nickname, then username, then the numeric id, matching how the Flutter client
// renders the profile (MyAccountCard: nickname, else username).
std::string fetchUsername(HttpClient& http, const std::string& baseUrl,
                          const std::string& token) {
    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = buildAuthUrl(baseUrl, "/v1/user/info");
    req.bearerToken = token;
    auto resp = http.send(req);
    if (!resp.ok || resp.status < kHttpOkMin || resp.status >= kHttpOkMax) {
        return {};
    }
    UserInfo info = parseUserInfo(parseJson(resp.body));
    if (!info.valid()) {
        return {};
    }
    if (!info.nickname.empty()) {
        return info.nickname;
    }
    return info.username.empty() ? info.id : info.username;
}

void loginWorker(LoginUi* ui) {
    // ShellExecuteW runs on this worker thread, so init COM here (some shell
    // handlers need an apartment). The worker never calls back into the UI
    // thread synchronously, which is what lets teardown join() it safely.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HttpClient http;
    LoginCallbacks cb;
    cb.transport = [&http](const HttpRequest& r) { return http.send(r); };
    cb.openUrl = [](const std::string& url) {
        const std::wstring wide = utf8ToWide(url);
        const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        return rc > kShellExecOk;
    };
    cb.wait = [ui](int seconds) {
        return WaitForSingleObject(ui->cancelEvent,
                                   static_cast<DWORD>(seconds) *
                                       kMillisPerSecond) == WAIT_OBJECT_0;
    };
    cb.onProgress = [ui](LoginPhase phase) {
        PostMessageW(ui->hwnd, WM_LOGIN_PROGRESS, static_cast<WPARAM>(phase),
                     0);
    };

    LoginResult result = runLogin(ui->baseUrl, "linuxdo", "", ui->device, cb);

    ui->done.outcome = result.outcome;
    if (result.outcome == LoginOutcome::Success) {
        // Push the credential through Session so the in-memory snapshot every
        // page consults is updated atomically with the disk write -- a later
        // ApiClient::send (from any thread) picks up the new bearer without
        // ever rereading the file.
        ui->done.persisted = Session::instance().setTokens(result.token);
        ui->done.detail =
            fetchUsername(http, ui->baseUrl, result.token.accessToken);
    } else {
        ui->done.detail = result.message;
    }
    if (SUCCEEDED(comInit)) {
        CoUninitialize();
    }
    // Last touch of ui: the worker is done writing ui->done, so signal the UI
    // thread. If the window is already gone (external teardown) the message is
    // simply dropped -- nothing leaks, since done lives inside ui.
    PostMessageW(ui->hwnd, WM_LOGIN_DONE, 0, 0);
}

// Runs on the main thread: SDK console/popup calls are safe here.
void reportResult(const DoneResult& p) {
    switch (p.outcome) {
        case LoginOutcome::Success: {
            const std::string who = p.detail.empty() ? "(unknown)" : p.detail;
            console::print(("HE-Music: 登录成功，用户 " + who).c_str());
            if (!p.persisted) {
                console::print(
                    "HE-Music: 警告 — 凭证未能写入磁盘，下次启动需重新登录");
            }
            popup_message::g_show(("已登录：" + who).c_str(), "HE-Music");
            break;
        }
        case LoginOutcome::Cancelled:
            console::print("HE-Music: 登录已取消");
            break;
        default: {
            const std::string msg = p.detail.empty() ? "登录失败" : p.detail;
            console::print(("HE-Music: 登录失败 — " + msg).c_str());
            popup_message::g_show(("登录失败：" + msg).c_str(), "HE-Music");
            break;
        }
    }
}

void beginCancel(LoginUi* ui) {
    if (ui->cancelling) {
        return;
    }
    SetEvent(ui->cancelEvent);
    ui->cancelling = true;
    InvalidateRect(ui->hwnd, nullptr, FALSE);
}

// --- Drawing -------------------------------------------------------------

// Centered text format; cheap to rebuild each paint (transient dialog).
ComPtr<IDWriteTextFormat> makeFormat(const std::wstring& family, float size) {
    ComPtr<IDWriteTextFormat> fmt;
    IDWriteFactory* dw = d2d::dwriteFactory();
    if (dw == nullptr) {
        return nullptr;
    }
    dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         size, L"", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return fmt;
}

void drawText(ID2D1RenderTarget* rt, const std::wstring& text,
              IDWriteTextFormat* fmt, const D2D1_RECT_F& rect,
              const D2D1_COLOR_F& color) {
    if (fmt == nullptr || text.empty()) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        return;
    }
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect,
                  brush.Get());
}

// 3/4 arc centered at the origin, swept clockwise; reused across paints (it is
// a device-independent resource) and rotated/translated into place per frame.
ID2D1PathGeometry* ringGeometry() {
    static ComPtr<ID2D1PathGeometry> geo = [] {
        ComPtr<ID2D1PathGeometry> g;
        ID2D1Factory* f = d2d::factory();
        if (f == nullptr || FAILED(f->CreatePathGeometry(g.GetAddressOf()))) {
            return ComPtr<ID2D1PathGeometry>{};
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g->Open(sink.GetAddressOf()))) {
            return ComPtr<ID2D1PathGeometry>{};
        }
        const float end = kRingSweepDeg * kDeg2Rad;
        sink->BeginFigure(D2D1::Point2F(kRingRadius, 0.0F),
                          D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(
            D2D1::Point2F(kRingRadius * std::cos(end),
                          kRingRadius * std::sin(end)),
            D2D1::SizeF(kRingRadius, kRingRadius), 0.0F,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        return g;
    }();
    return geo.Get();
}

void drawRing(ID2D1RenderTarget* rt, const Theme& theme, float centerX,
              float angle) {
    const D2D1_POINT_2F center = D2D1::Point2F(centerX, kRingCenterY);
    // Faint full-circle track.
    ComPtr<ID2D1SolidColorBrush> track;
    if (SUCCEEDED(rt->CreateSolidColorBrush(theme.secondaryText,
                                            track.GetAddressOf()))) {
        rt->DrawEllipse(D2D1::Ellipse(center, kRingRadius, kRingRadius),
                        track.Get(), kTrackStroke);
    }
    // Rotating accent arc on top.
    ID2D1PathGeometry* geo = ringGeometry();
    ComPtr<ID2D1SolidColorBrush> accent;
    if (geo == nullptr || FAILED(rt->CreateSolidColorBrush(
                              theme.selection, accent.GetAddressOf()))) {
        return;
    }
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle) *
                     D2D1::Matrix3x2F::Translation(center.x, center.y));
    rt->DrawGeometry(geo, accent.Get(), kRingStroke);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

D2D1_RECT_F buttonRect(float canvasW) {
    const float x = (canvasW - kBtnW) * kHalf;
    return D2D1::RectF(x, kBtnY, x + kBtnW, kBtnY + kBtnH);
}

void drawButton(ID2D1RenderTarget* rt, const Theme& theme, const LoginUi* ui,
                float canvasW) {
    const D2D1_RECT_F rect = buttonRect(canvasW);
    const D2D1_ROUNDED_RECT rr =
        D2D1::RoundedRect(rect, kBtnRadius, kBtnRadius);
    ComPtr<IDWriteTextFormat> fmt = makeFormat(theme.fontFamily, kBtnTextSize);

    const bool active = !ui->cancelling && (ui->btnHover || ui->btnPressed);
    if (active) {
        D2D1_COLOR_F fill = ui->btnPressed
                                ? mix(theme.selection, theme.text, kPressMix)
                                : theme.selection;
        ComPtr<ID2D1SolidColorBrush> fillBrush;
        if (SUCCEEDED(
                rt->CreateSolidColorBrush(fill, fillBrush.GetAddressOf()))) {
            rt->FillRoundedRectangle(rr, fillBrush.Get());
        }
        drawText(rt, L"取消", fmt.Get(), rect, theme.background);
    } else {
        // Outline + theme text; dimmed while cancelling (button is inert then).
        const D2D1_COLOR_F edge =
            ui->cancelling ? theme.secondaryText : theme.text;
        ComPtr<ID2D1SolidColorBrush> edgeBrush;
        if (SUCCEEDED(rt->CreateSolidColorBrush(theme.secondaryText,
                                                edgeBrush.GetAddressOf()))) {
            rt->DrawRoundedRectangle(rr, edgeBrush.Get(), 1.0F);
        }
        drawText(rt, L"取消", fmt.Get(), rect, edge);
    }
}

void onPaint(LoginUi* ui) {
    if (!ui->canvas) {
        ValidateRect(ui->hwnd, nullptr);
        return;
    }
    const Theme theme = themeFromHost();
    ui->canvas->paint([&](ID2D1HwndRenderTarget* rt) {
        const D2D1_SIZE_F size = rt->GetSize();
        rt->Clear(theme.background);

        ComPtr<IDWriteTextFormat> titleFmt =
            makeFormat(theme.fontFamily, kTitleSize);
        drawText(rt, L"登录 HE-Music", titleFmt.Get(),
                 D2D1::RectF(0.0F, kTitleY, size.width,
                             kTitleY + kTitleSize + kTitleBottomPad),
                 theme.text);

        drawRing(rt, theme, size.width * kHalf, ui->ringAngle);

        const std::wstring status =
            ui->cancelling ? L"正在取消…" : phaseText(ui->phase);
        ComPtr<IDWriteTextFormat> statusFmt =
            makeFormat(theme.fontFamily, kStatusSize);
        drawText(rt, status, statusFmt.Get(),
                 D2D1::RectF(0.0F, kStatusY, size.width, kStatusY + kStatusH),
                 theme.secondaryText);

        drawButton(rt, theme, ui, size.width);
    });
    ValidateRect(ui->hwnd, nullptr);
}

// --- Input ---------------------------------------------------------------

bool pointInButton(const LoginUi* ui, int px, int py, float canvasW) {
    const float dipX = static_cast<float>(px) / ui->dpiScale;
    const float dipY = static_cast<float>(py) / ui->dpiScale;
    const D2D1_RECT_F r = buttonRect(canvasW);
    return dipX >= r.left && dipX < r.right && dipY >= r.top && dipY < r.bottom;
}

float canvasWidthDip(const LoginUi* ui) {
    RECT rc{};
    if (GetClientRect(ui->hwnd, &rc) == 0) {
        return kDlgW;
    }
    return static_cast<float>(rc.right - rc.left) / ui->dpiScale;
}

void onMouseMove(LoginUi* ui, int px, int py) {
    if (!ui->tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, ui->hwnd, 0};
        TrackMouseEvent(&tme);
        ui->tracking = true;
    }
    const bool hover =
        !ui->cancelling && pointInButton(ui, px, py, canvasWidthDip(ui));
    if (hover != ui->btnHover) {
        ui->btnHover = hover;
        InvalidateRect(ui->hwnd, nullptr, FALSE);
    }
}

void onMouseLeave(LoginUi* ui) {
    ui->tracking = false;
    // btnPressed is owned by the capture lifecycle (released on button-up or
    // WM_CAPTURECHANGED), not by leave -- clearing it here without releasing
    // capture would strand the capture. Leave only drops hover.
    if (ui->btnHover) {
        ui->btnHover = false;
        InvalidateRect(ui->hwnd, nullptr, FALSE);
    }
}

// WM_CAPTURECHANGED: capture was lost (stolen by another window, or our own
// ReleaseCapture). Clear the pressed state so the button doesn't stick down.
void onCaptureChanged(LoginUi* ui) {
    if (ui->btnPressed) {
        ui->btnPressed = false;
        InvalidateRect(ui->hwnd, nullptr, FALSE);
    }
}

void onButtonDown(LoginUi* ui, int px, int py) {
    if (ui->cancelling || !pointInButton(ui, px, py, canvasWidthDip(ui))) {
        return;
    }
    ui->btnPressed = true;
    SetCapture(ui->hwnd);
    InvalidateRect(ui->hwnd, nullptr, FALSE);
}

void onButtonUp(LoginUi* ui, int px, int py) {
    if (!ui->btnPressed) {
        return;
    }
    ui->btnPressed = false;
    ReleaseCapture();
    const bool inside = pointInButton(ui, px, py, canvasWidthDip(ui));
    InvalidateRect(ui->hwnd, nullptr, FALSE);
    if (inside) {
        beginCancel(ui);
    }
}

void advanceRing(LoginUi* ui) {
    ui->ringAngle += kRingDegPerTick;
    if (ui->ringAngle >= kFullCircleDeg) {
        ui->ringAngle -= kFullCircleDeg;
    }
    InvalidateRect(ui->hwnd, nullptr, FALSE);
}

// Sole teardown path (WM_DESTROY). External destruction (fb2k shutdown / owner
// closing) can arrive while the worker is still polling, so signal cancel first
// or the join would hang waiting for a worker that never stops.
void destroyLoginUi(HWND hwnd, LoginUi* ui) {
    KillTimer(hwnd, kRingTimerId);
    if (ui->cancelEvent != nullptr) {
        SetEvent(ui->cancelEvent);
    }
    if (ui->worker.joinable()) {
        ui->worker.join();  // already joined on the DONE path
    }
    if (ui->canvas) {
        ui->canvas->discard();
    }
    if (ui->cancelEvent != nullptr) {
        CloseHandle(ui->cancelEvent);
    }
    if (g_active == ui) {
        g_active = nullptr;
    }
    delete ui;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
}

LRESULT CALLBACK loginWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui =
        reinterpret_cast<LoginUi*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (ui == nullptr) {
        // Pre-creation messages (before GWLP_USERDATA is set).
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // D2D paints the whole surface.
        case WM_PAINT:
            onPaint(ui);
            return 0;
        case WM_SIZE:
            if (ui->canvas) {
                ui->canvas->resize(LOWORD(lp), HIWORD(lp));
            }
            return 0;
        case WM_TIMER:
            if (wp == kRingTimerId) {
                advanceRing(ui);
            }
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            onMouseLeave(ui);
            return 0;
        case WM_LBUTTONDOWN:
            onButtonDown(ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            onButtonUp(ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_CAPTURECHANGED:
            onCaptureChanged(ui);
            return 0;
        case WM_LOGIN_PROGRESS:
            ui->phase = static_cast<LoginPhase>(wp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LOGIN_DONE:
            reportResult(ui->done);
            if (ui->worker.joinable()) {
                ui->worker.join();
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            // Don't destroy yet: signal cancel and wait for the worker's DONE,
            // which is the single teardown path.
            beginCancel(ui);
            return 0;
        case WM_DESTROY:
            destroyLoginUi(hwnd, ui);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ensureWindowClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = loginWndProc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No hbrBackground: D2D owns the whole client area.
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        return false;  // leave registered=false so a later attempt can retry
    }
    registered = true;
    return true;
}

float queryDpiScale() {
    HDC hdc = GetDC(nullptr);
    if (hdc == nullptr) {
        return 1.0F;
    }
    const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return dpi > 0 ? static_cast<float>(dpi) / kBaseDpi : 1.0F;
}

void centerOnOwner(HWND hwnd, HWND owner) {
    RECT self{};
    RECT ref{};
    if (GetWindowRect(hwnd, &self) == 0) {
        return;
    }
    const bool haveRef = owner != nullptr && GetWindowRect(owner, &ref) != 0;
    if (!haveRef) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0);
    }
    const int w = self.right - self.left;
    const int h = self.bottom - self.top;
    const int x = ref.left + ((ref.right - ref.left) - w) / 2;
    const int y = ref.top + ((ref.bottom - ref.top) - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

void showLoginDialog() {
    if (g_active != nullptr) {
        SetForegroundWindow(g_active->hwnd);
        return;
    }
    if (!ensureWindowClass()) {
        console::print("HE-Music: 无法注册登录窗口类");
        return;
    }

    auto ui = std::make_unique<LoginUi>();
    ui->cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ui->cancelEvent == nullptr) {
        console::print("HE-Music: 无法创建取消事件");
        return;
    }
    // Read config on the main thread (cfg_var / core_api), hand plain data off.
    ui->baseUrl = config::apiBaseUrl();
    ui->device =
        makeDeviceInfo(config::deviceId(), queryComputerName(), kAppVersion);
    ui->dpiScale = queryDpiScale();

    // The window is sized in physical pixels = DIP layout * DPI scale, so the
    // D2D render target (desktop DPI) reports our DIP layout via GetSize().
    const int pxW = static_cast<int>(kDlgW * ui->dpiScale);
    const int pxH = static_cast<int>(kDlgH * ui->dpiScale);

    HINSTANCE inst = core_api::get_my_instance();
    auto* owner = reinterpret_cast<HWND>(core_api::get_main_window());
    // CreateWindow size includes the non-client frame; expand so the client
    // area matches our intended DIP canvas.
    RECT want{0, 0, pxW, pxH};
    AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME);
    HWND hwnd =
        CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"HE-Music 登录",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                        CW_USEDEFAULT, want.right - want.left,
                        want.bottom - want.top, owner, nullptr, inst, nullptr);
    if (hwnd == nullptr) {
        CloseHandle(ui->cancelEvent);
        console::print("HE-Music: 无法创建登录窗口");
        return;
    }
    ui->hwnd = hwnd;
    ui->canvas = std::make_unique<d2d::HwndCanvas>(hwnd);

    LoginUi* raw = ui.release();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(raw));
    g_active = raw;

    centerOnOwner(hwnd, owner);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, kRingTimerId, kRingTimerMs, nullptr);

    raw->worker = std::thread(loginWorker, raw);
}

namespace {

mainmenu_group_popup_factory g_menuGroup(
    kGuidMenuGroup, mainmenu_groups::file,
    mainmenu_commands::sort_priority_dontcare, "HE-Music");

class HemusicMainMenu : public mainmenu_commands {
   public:
    t_uint32 get_command_count() override { return 1; }
    GUID get_command(t_uint32 /*index*/) override { return kGuidCmdLogin; }
    void get_name(t_uint32 /*index*/, pfc::string_base& out) override {
        out = "Login";
    }
    bool get_description(t_uint32 /*index*/, pfc::string_base& out) override {
        out = "Sign in to HE-Music via Linux.do.";
        return true;
    }
    GUID get_parent() override { return kGuidMenuGroup; }
    void execute(t_uint32 /*index*/,
                 service_ptr_t<service_base> /*callback*/) override {
        showLoginDialog();
    }
};

mainmenu_commands_factory_t<HemusicMainMenu> g_menuFactory;

}  // namespace

}  // namespace hemusic::ui
