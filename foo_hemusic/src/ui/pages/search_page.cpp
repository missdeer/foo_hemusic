#include "ui/pages/search_page.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "api/platforms.h"
#include "api/search.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/cover_cache.h"
#include "ui/image_cache.h"
#include "ui/pages/search_layout.h"
#include "ui/pages/section_render.h"

// No SDK headers: Session / ApiClient / HttpClient are SDK-free and the drawing
// is plain Win32 + Direct2D (shared via section_render.h).

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

// Resource-type label shown before a best-match item's detail (UTF-8, since it
// is concatenated with the UTF-8 sub detail before the row converts to wide).
std::string typeLabelUtf8(const std::string& rt) {
    if (rt == "song") {
        return "歌曲";
    }
    if (rt == "artist") {
        return "歌手";
    }
    if (rt == "playlist") {
        return "歌单";
    }
    if (rt == "album") {
        return "专辑";
    }
    if (rt == "mv") {
        return "视频";
    }
    return rt;
}

// All best-match variant types carry name + cover, so a generic visitor pulls
// them without per-type branches.
std::string bestName(const BestMatchItem& b) {
    return std::visit([](const auto& x) { return x.name; }, b.data);
}
std::string bestCover(const BestMatchItem& b) {
    return std::visit([](const auto& x) { return x.cover; }, b.data);
}

// Per-type secondary detail (artist names / song count / creator).
struct BestSubVisitor {
    std::string operator()(const SongInfo& s) const {
        return songArtistText(s);
    }
    std::string operator()(const ArtistInfo& a) const {
        return a.songCount + " 首";
    }
    std::string operator()(const PlaylistInfo& p) const {
        return p.songCount + " 首";
    }
    std::string operator()(const AlbumInfo& a) const {
        return artistNamesText(a.artists);
    }
    std::string operator()(const MvInfo& v) const { return v.creator; }
};

}  // namespace

SearchPage::~SearchPage() {
    // Worker writes members + PostMessages the (possibly already-dead) host.
    // Join before any member is destroyed so those writes stay valid (the sync
    // WinHTTP call can't be interrupted, so this may block up to the read
    // timeout -- bounded, like discover_page's teardown).
    if (worker_.joinable()) {
        worker_.join();
    }
    if (ImageCache* cache = coverCache()) {
        cache->unsubscribe(this);
    }
}

void SearchPage::search(const std::wstring& keyword) {
    // Trim and require a non-blank keyword.
    const std::wstring::size_type first = keyword.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return;
    }
    const std::wstring::size_type last = keyword.find_last_not_of(L" \t");
    const std::wstring trimmed = keyword.substr(first, last - first + 1);

    bool expected = false;
    if (!loading_.compare_exchange_strong(expected, true)) {
        return;  // a search is already in flight
    }

    scrollY_ = 0.0F;  // UI thread: reset to top for the new results

    // UTF-16 -> UTF-8 for the request + the echoed title.
    const int n = WideCharToMultiByte(CP_UTF8, 0, trimmed.data(),
                                      static_cast<int>(trimmed.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, trimmed.data(),
                        static_cast<int>(trimmed.size()), utf8.data(), n,
                        nullptr, nullptr);

    if (!Session::instance().isAuthenticated()) {
        {
            const std::lock_guard<std::mutex> lk(mu_);
            status_ = Status::NotLoggedIn;
            keyword_ = std::move(utf8);
            bestMatch_.clear();
            songs_.clear();
            playlists_.clear();
            albums_.clear();
            artists_.clear();
            videos_.clear();
            message_.clear();
        }
        loading_ = false;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Loading;
        keyword_ = utf8;
    }
    if (worker_.joinable()) {
        worker_.join();  // a previous finished worker; reuse the slot
    }
    worker_ = std::thread([this, utf8] { worker(utf8); });
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void SearchPage::reset() {
    if (loading_) {
        return;  // let the in-flight worker finish; it owns the state
    }
    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Idle;
        keyword_.clear();
        bestMatch_.clear();
        songs_.clear();
        playlists_.clear();
        albums_.clear();
        artists_.clear();
        videos_.clear();
        message_.clear();
    }
    scrollY_ = 0.0F;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void SearchPage::worker(std::string keyword) {
    HttpClient http;
    ApiClient::Transport transport = [&http](const HttpRequest& r) {
        return http.send(r);
    };

    Status st = Status::Error;
    std::string msg;
    ComprehensiveSearchResult result;

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
            auto platform = resolveSearchPlatform(platforms);
            if (!platform) {
                msg = "没有支持搜索的可用平台";
            } else {
                HttpRequest searchReq;
                searchReq.url = url::buildUrl(
                    base, "/v1/search",
                    {{"key", keyword}, {"platform", platform->id}});
                searchReq.connectTimeoutMs = kFetchConnectMs;
                searchReq.readTimeoutMs = kFetchReadMs;
                const HttpResponse searchResp = client.send(searchReq);
                if (!isHttpOk(searchResp)) {
                    msg = "搜索失败";
                } else {
                    result = parseComprehensiveSearch(
                        parseJson(searchResp.body), platform->id, keyword);
                    st = Status::Loaded;
                }
            }
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = st;
        message_ = std::move(msg);
        bestMatch_ = std::move(result.bestMatch);
        songs_ = std::move(result.song.items);
        playlists_ = std::move(result.playlist.items);
        albums_ = std::move(result.album.items);
        artists_ = std::move(result.artist.items);
        videos_ = std::move(result.video.items);
    }
    loading_ = false;
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool SearchPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
    if (msg == kDoneMessage) {
        scrollY_ = 0.0F;  // UI thread: fresh results start at the top
    } else if (msg != kCoverReadyMessage) {
        return false;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return true;
}

void SearchPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void SearchPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                       D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);

    switch (status_) {
        case Status::Idle:
            drawCentered(rt, theme, size, L"输入关键词搜索");
            return;
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在搜索…");
            return;
        case Status::NotLoggedIn:
            drawCentered(rt, theme, size,
                         L"请先登录 HE-Music（文件 > HE-Music > Login）");
            return;
        case Status::Error:
            drawCentered(rt, theme, size,
                         utf8ToWide(message_.empty() ? "搜索失败" : message_));
            return;
        case Status::Loaded:
            break;
    }

    if (bestMatch_.empty() && songs_.empty() && playlists_.empty() &&
        albums_.empty() && artists_.empty() && videos_.empty()) {
        drawCentered(rt, theme, size,
                     utf8ToWide(keyword_) + L"：未找到相关结果");
        return;
    }

    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;

    const SearchLayout layout = computeSearchLayout(
        bestMatch_.size(), songs_.size(), playlists_.size(), albums_.size(),
        artists_.size(), videos_.size(), size.width, m);
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
    rn.covers = coverCache();
    rn.subscriber = this;
    rn.host = host_;
    rn.coverReadyMsg = kCoverReadyMessage;

    // Keyword title ("<keyword> 搜索结果"), scrolling with the content.
    const D2D1_RECT_F titleRect = rn.screen(layout.keywordTitle);
    if (rn.visible(titleRect)) {
        drawText(rt, rn.titleFmt.Get(), utf8ToWide(keyword_) + L" 搜索结果",
                 rn.textBrush.Get(), titleRect);
    }

    drawSongListSection(
        rn, layout.bestMatch, bestMatch_, L"最佳匹配",
        [](const BestMatchItem& b) { return bestName(b); },
        [](const BestMatchItem& b) {
            const std::string sub = std::visit(BestSubVisitor{}, b.data);
            const std::string label = typeLabelUtf8(b.resourceType);
            return sub.empty() ? label : label + " · " + sub;
        },
        [](const BestMatchItem& b) { return bestCover(b); });
    drawSongListSection(
        rn, layout.song, songs_, L"歌曲",
        [](const SongInfo& s) { return s.name; },
        [](const SongInfo& s) { return songArtistText(s); },
        [](const SongInfo& s) { return s.cover; });
    drawCardSection(
        rn, layout.playlist, playlists_, L"歌单", /*square=*/true,
        [](const PlaylistInfo& p) { return p.name; },
        [](const PlaylistInfo& p) { return p.songCount + " 首"; },
        [](const PlaylistInfo& p) { return p.cover; });
    drawCardSection(
        rn, layout.album, albums_, L"专辑", /*square=*/true,
        [](const AlbumInfo& a) { return a.name; },
        [](const AlbumInfo& a) { return artistNamesText(a.artists); },
        [](const AlbumInfo& a) { return a.cover; });
    drawCardSection(
        rn, layout.artist, artists_, L"歌手", /*square=*/true,
        [](const ArtistInfo& a) { return a.name; },
        [](const ArtistInfo& a) { return a.songCount + " 首"; },
        [](const ArtistInfo& a) { return a.cover; });
    drawCardSection(
        rn, layout.video, videos_, L"视频", /*square=*/false,
        [](const MvInfo& v) { return v.name; },
        [](const MvInfo& v) { return v.creator; },
        [](const MvInfo& v) { return v.cover; });
}

}  // namespace hemusic::ui
