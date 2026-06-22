#include "ui/pages/discover_page.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "api/discover.h"
#include "api/platforms.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/d2d.h"

// No SDK headers here: Session / ApiClient / HttpClient are SDK-free and d2d /
// theme are plain Win32 + Direct2D, so nothing pulls WinSock2 and the lone
// <windows.h> is unambiguous.

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;

constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;  // exclusive (2xx)

// Layout factors (DIPs / multipliers) beyond the Theme constants.
constexpr float kSubGap = 2.0F;            // title line -> artist line
constexpr float kSectionTitleBand = 1.6F;  // title height as a factor of size

bool isHttpOk(const HttpResponse& r) {
    return r.ok && r.status >= kHttpOkMin && r.status < kHttpOkMax;
}

boost::json::value parseJson(const std::string& s) {
    boost::system::error_code ec;
    auto v = boost::json::parse(s, ec);
    return ec ? boost::json::value(nullptr) : v;
}

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
                                     bool centered) {
    ComPtr<IDWriteTextFormat> tf;
    IDWriteFactory* dw = d2d::dwriteFactory();
    if (dw == nullptr) {
        return tf;
    }
    dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         size, L"", tf.GetAddressOf());
    if (tf) {
        tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (centered) {
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    return tf;
}

void drawText(ID2D1RenderTarget* rt, IDWriteTextFormat* tf,
              const std::wstring& text, const D2D1_COLOR_F& color,
              const D2D1_RECT_F& rect) {
    if (tf == nullptr || text.empty()) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        return;
    }
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), tf, rect,
                  brush.Get());
}

}  // namespace

DiscoverPage::~DiscoverPage() {
    // Worker writes members + PostMessages the (possibly already-dead) host.
    // Join before any member is destroyed so those writes stay valid; the sync
    // WinHTTP call can't be interrupted, so this may block up to the read
    // timeout (bounded, like login_dlg's teardown).
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DiscoverPage::load() {
    bool expected = false;
    if (!loading_.compare_exchange_strong(expected, true)) {
        return;  // a load is already in flight
    }

    if (!Session::instance().isAuthenticated()) {
        {
            const std::lock_guard<std::mutex> lk(mu_);
            status_ = Status::NotLoggedIn;
            songs_.clear();
            message_.clear();
        }
        loading_ = false;
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Loading;
    }
    if (worker_.joinable()) {
        worker_.join();  // a previous finished worker; reuse the slot
    }
    worker_ = std::thread([this] { worker(); });
}

void DiscoverPage::worker() {
    HttpClient http;
    ApiClient::Transport transport = [&http](const HttpRequest& r) {
        return http.send(r);
    };

    Status st = Status::Error;
    std::string msg;
    std::vector<SongInfo> songs;

    Session& session = Session::instance();
    auto clientOpt = session.buildClient(transport);
    if (!clientOpt) {
        msg = "会话未初始化";
    } else {
        ApiClient client = std::move(*clientOpt);
        const std::string base = session.baseUrl();

        HttpRequest platReq;
        platReq.url = url::buildUrl(base, "/v1/platforms");
        const HttpResponse platResp = client.send(platReq);
        if (!isHttpOk(platResp)) {
            msg = "无法获取平台列表";
        } else {
            auto platforms = parsePlatformList(parseJson(platResp.body));
            auto platform = resolveDiscoverPlatform(platforms);
            if (!platform) {
                msg = "没有支持发现页的可用平台";
            } else {
                HttpRequest discReq;
                discReq.url = url::buildUrl(base, "/v1/page/discover",
                                            {{"platform", platform->id}});
                const HttpResponse discResp = client.send(discReq);
                if (!isHttpOk(discResp)) {
                    msg = "无法获取发现页";
                } else {
                    auto page = parseDiscoverPage(parseJson(discResp.body),
                                                  platform->id);
                    songs = std::move(page.newSongs);
                    st = Status::Loaded;
                }
            }
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = st;
        message_ = std::move(msg);
        songs_ = std::move(songs);
    }
    loading_ = false;
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool DiscoverPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
    if (msg != kDoneMessage) {
        return false;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return true;
}

void DiscoverPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                         D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);

    auto centered = [&](const std::wstring& text) {
        ComPtr<IDWriteTextFormat> tf =
            makeFormat(theme.fontFamily, theme.rowTitleSize, /*centered=*/true);
        drawText(rt, tf.Get(), text, theme.secondaryText,
                 D2D1::RectF(0.0F, 0.0F, size.width, size.height));
    };

    switch (status_) {
        case Status::Idle:
        case Status::Loading:
            centered(L"正在加载发现页…");
            return;
        case Status::NotLoggedIn:
            centered(L"请先登录 HE-Music（文件 > HE-Music > Login）");
            return;
        case Status::Error:
            centered(utf8ToWide(message_.empty() ? "加载失败" : message_));
            return;
        case Status::Loaded:
            break;
    }

    if (songs_.empty()) {
        centered(L"发现页暂无新歌");
        return;
    }

    ComPtr<IDWriteTextFormat> titleFmt = makeFormat(
        theme.fontFamily, theme.sectionTitleSize, /*centered=*/false);
    ComPtr<IDWriteTextFormat> rowFmt =
        makeFormat(theme.fontFamily, theme.rowTitleSize, /*centered=*/false);
    ComPtr<IDWriteTextFormat> subFmt =
        makeFormat(theme.fontFamily, theme.rowSubSize, /*centered=*/false);

    const float left = theme.padding;
    const float right = size.width - theme.padding;
    float y = theme.padding;

    const float bandHeight = theme.sectionTitleSize * kSectionTitleBand;
    drawText(rt, titleFmt.Get(), L"新歌", theme.text,
             D2D1::RectF(left, y, right, y + bandHeight));
    y += bandHeight;

    for (const SongInfo& song : songs_) {
        if (y + theme.rowHeight > size.height) {
            break;  // no scrolling yet (Phase 5); stop at the visible bottom
        }
        const float subY = y + theme.rowTitleSize + kSubGap;
        drawText(rt, rowFmt.Get(), utf8ToWide(song.name), theme.text,
                 D2D1::RectF(left, y, right, subY));
        drawText(rt, subFmt.Get(), utf8ToWide(songArtistText(song)),
                 theme.secondaryText,
                 D2D1::RectF(left, subY, right, y + theme.rowHeight));
        y += theme.rowHeight;
    }
}

}  // namespace hemusic::ui
