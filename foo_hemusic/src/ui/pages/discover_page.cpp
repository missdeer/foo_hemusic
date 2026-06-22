#include "ui/pages/discover_page.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "api/discover.h"
#include "api/platforms.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/pages/discover_layout.h"
#include "ui/pages/section_render.h"

// No SDK headers here: Session / ApiClient / HttpClient are SDK-free and d2d /
// theme are plain Win32 + Direct2D, so nothing pulls WinSock2 and the lone
// <windows.h> is unambiguous. The shared section drawing (Renderer / card +
// song-list sections / makeFormat / drawCentered) lives in section_render.h.

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;
using render_detail::isHttpOk;
using render_detail::kFetchConnectMs;
using render_detail::kFetchReadMs;
using render_detail::parseJson;

// Pixels scrolled per wheel notch (WHEEL_DELTA units).
constexpr float kScrollStepPx = 120.0F;
// Section title band height as a factor of the title font size.
constexpr float kTitleBandFactor = 1.6F;

}  // namespace

DiscoverPage::~DiscoverPage() {
    // Worker writes members + PostMessages the (possibly already-dead) host.
    // Join before any member is destroyed so those writes stay valid; the sync
    // WinHTTP call can't be interrupted, so this may block up to the read
    // timeout (bounded, like login_dlg's teardown).
    if (worker_.joinable()) {
        worker_.join();
    }
    // Then stop cover notifications addressed to us. Order matters: the worker
    // never touches the cache, but unsubscribing only after the join keeps the
    // teardown sequence unambiguous. Late cover listeners only PostMessage a
    // dead HWND anyway (harmless), but dropping the subscription stops the
    // cache from holding our stale listeners.
    if (ImageCache* cache = coverCache()) {
        cache->unsubscribe(this);
    }
}

void DiscoverPage::load() {
    bool expected = false;
    if (!loading_.compare_exchange_strong(expected, true)) {
        return;  // a load is already in flight
    }

    scrollY_ = 0.0F;  // UI thread: reset to top for the new data

    if (!Session::instance().isAuthenticated()) {
        {
            const std::lock_guard<std::mutex> lk(mu_);
            status_ = Status::NotLoggedIn;
            songs_.clear();
            albums_.clear();
            playlists_.clear();
            mvs_.clear();
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
    // hemusic::DiscoverPage is the api/ response model (distinct from this UI
    // class of the same name); qualify it to disambiguate.
    hemusic::DiscoverPage page;

    Session& session = Session::instance();
    auto clientOpt = session.buildClient(transport);
    if (!clientOpt) {
        msg = "会话未初始化";
    } else {
        ApiClient client = std::move(*clientOpt);
        const std::string base = session.baseUrl();

        HttpRequest platReq;
        platReq.url = url::buildUrl(base, "/v1/platforms");
        platReq.connectTimeoutMs = kFetchConnectMs;
        platReq.readTimeoutMs = kFetchReadMs;
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
                discReq.connectTimeoutMs = kFetchConnectMs;
                discReq.readTimeoutMs = kFetchReadMs;
                const HttpResponse discResp = client.send(discReq);
                if (!isHttpOk(discResp)) {
                    msg = "无法获取发现页";
                } else {
                    page = parseDiscoverPage(parseJson(discResp.body),
                                             platform->id);
                    st = Status::Loaded;
                }
            }
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = st;
        message_ = std::move(msg);
        songs_ = std::move(page.newSongs);
        albums_ = std::move(page.newAlbums);
        playlists_ = std::move(page.featuredPlaylists);
        mvs_ = std::move(page.featuredMvs);
    }
    loading_ = false;
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool DiscoverPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
    if (msg == kDoneMessage) {
        scrollY_ = 0.0F;  // UI thread: fresh data starts at the top
    } else if (msg != kCoverReadyMessage) {
        return false;
    }
    // A cover that just finished loading must repaint in place -- only fresh
    // page data (kDoneMessage) resets the scroll above.
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return true;
}

void DiscoverPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void DiscoverPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                         D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);

    switch (status_) {
        case Status::Idle:
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在加载发现页…");
            return;
        case Status::NotLoggedIn:
            drawCentered(rt, theme, size,
                         L"请先登录 HE-Music（文件 > HE-Music > Login）");
            return;
        case Status::Error:
            drawCentered(rt, theme, size,
                         utf8ToWide(message_.empty() ? "加载失败" : message_));
            return;
        case Status::Loaded:
            break;
    }

    if (songs_.empty() && albums_.empty() && playlists_.empty() &&
        mvs_.empty()) {
        drawCentered(rt, theme, size, L"发现页暂无内容");
        return;
    }

    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;

    const DiscoverLayout layout =
        computeLayout(songs_.size(), albums_.size(), playlists_.size(),
                      mvs_.size(), size.width, m);
    contentHeight_ = layout.contentHeight;
    const float maxScroll = std::max(0.0F, contentHeight_ - size.height);
    scrollY_ = std::clamp(scrollY_, 0.0F, maxScroll);

    Renderer rn;
    rn.rt = rt;
    rn.titleFmt = makeFormat(theme.fontFamily, theme.sectionTitleSize, false,
                             /*ellipsis=*/true);
    rn.rowFmt = makeFormat(theme.fontFamily, theme.rowTitleSize, false,
                           /*ellipsis=*/true);
    rn.subFmt = makeFormat(theme.fontFamily, theme.rowSubSize, false,
                           /*ellipsis=*/true);
    if (FAILED(rt->CreateSolidColorBrush(theme.text,
                                         rn.textBrush.GetAddressOf())) ||
        FAILED(rt->CreateSolidColorBrush(theme.secondaryText,
                                         rn.subBrush.GetAddressOf()))) {
        return;
    }
    rn.scrollY = scrollY_;
    rn.viewportH = size.height;
    rn.rowTitleH = theme.rowTitleSize + render_detail::kSubGap;
    rn.covers =
        coverCache();  // null before init / after shutdown -> placeholder
    rn.subscriber = this;
    rn.host = host_;
    rn.coverReadyMsg = kCoverReadyMessage;

    drawSongListSection(
        rn, layout.songs, songs_, L"新歌速递",
        [](const SongInfo& s) { return s.name; },
        [](const SongInfo& s) { return songArtistText(s); },
        [](const SongInfo& s) { return s.cover; });
    drawCardSection(
        rn, layout.albums, albums_, L"新碟上架", /*square=*/true,
        [](const AlbumInfo& a) { return a.name; },
        [](const AlbumInfo& a) { return artistNamesText(a.artists); },
        [](const AlbumInfo& a) { return a.cover; });
    drawCardSection(
        rn, layout.playlists, playlists_, L"精选歌单", /*square=*/true,
        [](const PlaylistInfo& p) { return p.name; },
        [](const PlaylistInfo& p) { return p.songCount + " 首"; },
        [](const PlaylistInfo& p) { return p.cover; });
    drawCardSection(
        rn, layout.mvs, mvs_, L"精选 MV", /*square=*/false,
        [](const MvInfo& v) { return v.name; },
        [](const MvInfo& v) { return v.creator; },
        [](const MvInfo& v) { return v.cover; });
}

}  // namespace hemusic::ui
