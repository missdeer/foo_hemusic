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

#include <filesystem>
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
#include "net/http_client.h"

// Threading model: the menu command runs on fb2k's main thread and creates this
// modeless window there, so fb2k's message pump dispatches its messages. The
// blocking login (WinHTTP) runs on a worker std::thread; it talks back to the
// UI only via PostMessage (fire-and-forget) -- never a synchronous SendMessage
// -- so the UI thread can join the worker without risking a cross-thread
// deadlock:
//   - PostMessage WM_LOGIN_PROGRESS  -> update status text
//   - PostMessage WM_LOGIN_DONE      -> report result + tear down
// openUrl (ShellExecuteW) runs on the worker itself (COM-initialized). Teardown
// always sets the cancel event before joining, so a worker mid-poll exits
// promptly instead of hanging the join. Cancellation is a manual-reset event
// the WaitFn polls between status polls.

namespace hemusic::ui {

namespace {

constexpr const char* kAppVersion = "0.0.1";  // mirrors component.cpp
constexpr wchar_t kWindowClass[] = L"foo_hemusic_login";

constexpr UINT WM_LOGIN_PROGRESS = WM_APP + 1;
constexpr UINT WM_LOGIN_DONE = WM_APP + 2;

constexpr int kIdCancel = 1;

constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;          // exclusive (2xx)
constexpr DWORD kMillisPerSecond = 1000;  // WaitForSingleObject unit
constexpr INT_PTR kShellExecOk = 32;      // ShellExecuteW success is > 32

// Window / control layout (device-independent pixels at 96 DPI).
constexpr int kDlgWidth = 380;
constexpr int kDlgHeight = 150;
constexpr int kMargin = 20;
constexpr int kStatusWidth = 340;
constexpr int kStatusHeight = 40;
constexpr int kBtnWidth = 80;
constexpr int kBtnHeight = 28;
constexpr int kBtnX = 150;
constexpr int kBtnY = 75;

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
            return L"请在浏览器中完成授权…";
        case LoginPhase::Finalizing:
            return L"授权成功，正在获取账号…";
    }
    return L"";
}

// State for one open dialog; owned by the window (GWLP_USERDATA), freed in
// WM_DESTROY after the worker has joined. Single instance via g_active.
struct LoginUi {
    HWND hwnd = nullptr;
    HWND status = nullptr;
    HWND cancelBtn = nullptr;
    HANDLE cancelEvent = nullptr;  // manual-reset
    std::thread worker;

    // Captured on the main thread so the worker never touches cfg_var/core_api.
    std::string baseUrl;
    DeviceInfo device;
    std::filesystem::path tokenPath;
};

LoginUi* g_active = nullptr;  // main-thread only

// Heap payload handed to WM_LOGIN_DONE (lParam); the handler takes ownership.
struct DonePayload {
    LoginOutcome outcome = LoginOutcome::TransportError;
    std::string detail;     // username on success, else error message
    bool persisted = true;  // whether the token reached disk (success path)
};

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

    auto payload = std::make_unique<DonePayload>();
    payload->outcome = result.outcome;
    if (result.outcome == LoginOutcome::Success) {
        payload->persisted = TokenStore(ui->tokenPath).save(result.token);
        payload->detail =
            fetchUsername(http, ui->baseUrl, result.token.accessToken);
    } else {
        payload->detail = result.message;
    }
    if (SUCCEEDED(comInit)) {
        CoUninitialize();
    }
    // Last touch of ui: after this the UI thread may destroy it.
    PostMessageW(ui->hwnd, WM_LOGIN_DONE, 0,
                 reinterpret_cast<LPARAM>(payload.release()));
}

// Runs on the main thread: SDK console/popup calls are safe here.
void reportResult(const DonePayload& p) {
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
    SetEvent(ui->cancelEvent);
    EnableWindow(ui->cancelBtn, FALSE);
    SetWindowTextW(ui->status, L"正在取消…");
}

LRESULT CALLBACK loginWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui =
        reinterpret_cast<LoginUi*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_LOGIN_PROGRESS:
            if (ui != nullptr) {
                SetWindowTextW(ui->status,
                               phaseText(static_cast<LoginPhase>(wp)));
            }
            return 0;
        case WM_LOGIN_DONE: {
            std::unique_ptr<DonePayload> payload(
                reinterpret_cast<DonePayload*>(lp));
            reportResult(*payload);
            if (ui != nullptr && ui->worker.joinable()) {
                ui->worker.join();
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kIdCancel && ui != nullptr) {
                beginCancel(ui);
            }
            return 0;
        case WM_CLOSE:
            // Don't destroy yet: signal cancel and wait for the worker's DONE,
            // which is the single teardown path.
            if (ui != nullptr) {
                beginCancel(ui);
            }
            return 0;
        case WM_DESTROY:
            if (ui != nullptr) {
                // External destruction (fb2k shutdown / owner closing) can
                // reach here while the worker is still polling: signal cancel
                // first so the join doesn't hang waiting for a worker that
                // never stops.
                if (ui->cancelEvent != nullptr) {
                    SetEvent(ui->cancelEvent);
                }
                if (ui->worker.joinable()) {
                    ui->worker.join();  // already joined on the DONE path
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
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensureWindowClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = loginWndProc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    registered = true;
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
    ensureWindowClass();

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
    ui->tokenPath = config::tokenStorePath();

    HINSTANCE inst = core_api::get_my_instance();
    auto* owner = reinterpret_cast<HWND>(core_api::get_main_window());
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kWindowClass, L"HE-Music 登录",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        kDlgWidth, kDlgHeight, owner, nullptr, inst, nullptr);
    if (hwnd == nullptr) {
        CloseHandle(ui->cancelEvent);
        console::print("HE-Music: 无法创建登录窗口");
        return;
    }
    ui->hwnd = hwnd;
    ui->status = CreateWindowExW(
        0, L"STATIC", L"正在连接 HE-Music…", WS_CHILD | WS_VISIBLE, kMargin,
        kMargin, kStatusWidth, kStatusHeight, hwnd, nullptr, inst, nullptr);
    ui->cancelBtn = CreateWindowExW(
        0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, kBtnX,
        kBtnY, kBtnWidth, kBtnHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdCancel)), inst,
        nullptr);

    auto* font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(ui->status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(ui->cancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                 TRUE);

    LoginUi* raw = ui.release();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(raw));
    g_active = raw;

    centerOnOwner(hwnd, owner);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

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
