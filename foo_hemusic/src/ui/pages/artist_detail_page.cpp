#include "ui/pages/artist_detail_page.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "api/album.h"
#include "api/artist.h"
#include "api/artist_detail.h"
#include "api/mv.h"
#include "api/song.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/cover_cache.h"
#include "ui/image_cache.h"
#include "ui/pages/detail_layout.h"
#include "ui/pages/section_render.h"

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;
using render_detail::isHttpOk;
using render_detail::kFetchConnectMs;
using render_detail::kFetchReadMs;
using render_detail::parseJson;

constexpr float kScrollStepPx = 120.0F;
constexpr float kTitleBandFactor = 1.6F;
constexpr float kBannerTitleScale = 1.25F;
constexpr float kBannerSubGap = 4.0F;

// Eager-fetch guard (matches Flutter `_fetchAllPages`). Most artists fit in 1
// page (page_size=500 for songs, 200 for albums/mvs); the cap protects against
// runaway pagination on a server bug or a popular artist with 10k tracks.
constexpr int kMaxPages = 20;
constexpr int kSongsPageSize = 500;
constexpr int kCardsPageSize = 200;

const std::array<const wchar_t*, 3> kTabLabels = {L"歌曲", L"专辑", L"MV"};

LayoutMetrics currentMetrics(const Theme& theme) {
    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;
    return m;
}

bool rectContains(const LayoutRect& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// "<alias> · <song_count> 首 · <album_count> 张专辑 · <mv_count> 个 MV"
std::string formatArtistMeta(const ArtistInfo& a) {
    std::string out;
    const auto append = [&](const std::string& s) {
        if (s.empty()) {
            return;
        }
        if (!out.empty()) {
            out += " · ";
        }
        out += s;
    };
    if (!a.alias.empty()) {
        append(a.alias);
    }
    if (!a.songCount.empty() && a.songCount != "0") {
        append(a.songCount + " 首");
    }
    if (!a.albumCount.empty() && a.albumCount != "0") {
        append(a.albumCount + " 张专辑");
    }
    if (!a.mvCount.empty() && a.mvCount != "0") {
        append(a.mvCount + " 个 MV");
    }
    return out;
}

void paintBannerInfo(ID2D1RenderTarget* rt, const Theme& theme,
                     const Renderer& rn, IDWriteTextFormat* bannerTitleFmt,
                     const D2D1_RECT_F& infoS, const ArtistInfo& a) {
    const float titleH =
        theme.sectionTitleSize * kBannerTitleScale + kBannerSubGap;
    const float subLineH = theme.rowSubSize + kBannerSubGap;
    float y = infoS.top + kBannerSubGap;

    drawText(rt, bannerTitleFmt, utf8ToWide(a.name), rn.textBrush.Get(),
             D2D1::RectF(infoS.left, y, infoS.right, y + titleH));
    y += titleH;

    const std::string meta = formatArtistMeta(a);
    if (!meta.empty()) {
        drawText(rt, rn.subFmt.Get(), utf8ToWide(meta), rn.subBrush.Get(),
                 D2D1::RectF(infoS.left, y, infoS.right, y + subLineH));
        y += subLineH;
    }
    if (!a.description.empty() && y + subLineH <= infoS.bottom) {
        // Description gets whatever vertical room is left; the textFormat has
        // ellipsis trimming so a long bio is clipped to one line.
        drawText(rt, rn.subFmt.Get(), utf8ToWide(a.description),
                 rn.subBrush.Get(),
                 D2D1::RectF(infoS.left, y, infoS.right, infoS.bottom));
    }
}

void paintTabStrip(ID2D1RenderTarget* rt, const Theme& theme,
                   const Renderer& rn, IDWriteTextFormat* tabFmt,
                   const ArtistTabBarLayout& bar,
                   ID2D1SolidColorBrush* selBrush, ArtistTab active) {
    // Underline parameters mirror MainPanel's top tab bar (kUnderline*) so the
    // visual treatment of "active tab" is consistent across the panel.
    constexpr float kUnderlineH = 2.0F;
    constexpr float kUnderlineInset = 14.0F;
    for (std::size_t i = 0; i < bar.tabs.size(); ++i) {
        const LayoutRect& r = bar.tabs.at(i);
        const D2D1_RECT_F screen = rn.screen(r);
        if (!rn.visible(screen)) {
            continue;
        }
        const bool isActive = static_cast<std::size_t>(active) == i;
        drawText(rt, tabFmt, kTabLabels.at(i),
                 isActive ? rn.textBrush.Get() : rn.subBrush.Get(), screen);
        if (isActive && selBrush != nullptr) {
            rt->FillRectangle(
                D2D1::RectF(screen.left + kUnderlineInset,
                            screen.bottom - kUnderlineH,
                            screen.right - kUnderlineInset, screen.bottom),
                selBrush);
        }
    }
    // Thin separator under the whole strip.
    const D2D1_RECT_F sep = rn.screen({bar.band.left, bar.band.bottom - 1.0F,
                                       bar.band.right, bar.band.bottom});
    if (rn.visible(sep)) {
        rt->FillRectangle(sep, rn.subBrush.Get());
    }
    (void)theme;
}

}  // namespace

ArtistDetailPage::~ArtistDetailPage() {
    gen_.fetch_add(1);
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    if (ImageCache* cache = coverCache()) {
        cache->unsubscribe(this);
    }
}

void ArtistDetailPage::enter(const nav::PageEntry& entry) {
    const auto pickParam = [&](const char* key) -> std::string {
        auto it = entry.params.find(key);
        return it == entry.params.end() ? std::string{} : it->second;
    };
    WorkerArgs args{pickParam("id"), pickParam("platform"), pickParam("title")};
    if (args.title.empty()) {
        args.title = entry.title;
    }

    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    activeTab_ = ArtistTab::Songs;

    const std::uint64_t myGen = gen_.fetch_add(1) + 1;

    if (args.id.empty() || args.platform.empty()) {
        const std::lock_guard<std::mutex> lk(mu_);
        bannerStatus_ = Status::Error;
        bannerMessage_ = "歌手参数缺失";
        info_ = ArtistInfo{};
        info_.name = args.title;
        songs_.clear();
        albums_.clear();
        mvs_.clear();
        songsState_ = albumsState_ = mvsState_ = ListStatus{};
        args_ = args;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    if (!Session::instance().isAuthenticated()) {
        const std::lock_guard<std::mutex> lk(mu_);
        bannerStatus_ = Status::NotLoggedIn;
        bannerMessage_.clear();
        info_ = ArtistInfo{};
        info_.name = args.title;
        info_.id = args.id;
        info_.platform = args.platform;
        songs_.clear();
        albums_.clear();
        mvs_.clear();
        songsState_ = albumsState_ = mvsState_ = ListStatus{};
        args_ = args;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        bannerStatus_ = Status::Loading;
        bannerMessage_.clear();
        info_ = ArtistInfo{};
        info_.name = args.title;
        info_.id = args.id;
        info_.platform = args.platform;
        songs_.clear();
        albums_.clear();
        mvs_.clear();
        songsState_ = albumsState_ = mvsState_ = ListStatus{};
        songsState_.status = Status::Loading;
        songsState_.started = true;
        args_ = args;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    workers_.emplace_back([this, args, myGen] { bannerWorker(args, myGen); });
    workers_.emplace_back([this, args, myGen] { songsWorker(args, myGen); });
}

void ArtistDetailPage::reset() {
    gen_.fetch_add(1);
    {
        const std::lock_guard<std::mutex> lk(mu_);
        bannerStatus_ = Status::Idle;
        bannerMessage_.clear();
        info_ = ArtistInfo{};
        songs_.clear();
        albums_.clear();
        mvs_.clear();
        songsState_ = albumsState_ = mvsState_ = ListStatus{};
        args_ = WorkerArgs{};
    }
    activeTab_ = ArtistTab::Songs;
    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

namespace {

// Builds an ApiClient bound to the live Session. Returns nullopt if the
// session is not configured (caller should report "会话未初始化").
std::optional<ApiClient> buildSessionClient(HttpClient& http) {
    ApiClient::Transport transport = [&http](const HttpRequest& r) {
        return http.send(r);
    };
    return Session::instance().buildClient(transport);
}

HttpResponse fetchJson(ApiClient& client, const std::string& url) {
    HttpRequest req;
    req.url = url;
    req.connectTimeoutMs = kFetchConnectMs;
    req.readTimeoutMs = kFetchReadMs;
    return client.send(req);
}

}  // namespace

void ArtistDetailPage::bannerWorker(WorkerArgs args, std::uint64_t myGen) {
    HttpClient http;
    ArtistInfo info;
    info.id = args.id;
    info.platform = args.platform;
    info.name = args.title;
    std::vector<SongInfo> embeddedSongs;
    Status st = Status::Error;
    std::string msg;

    auto clientOpt = buildSessionClient(http);
    if (!clientOpt) {
        msg = "会话未初始化";
    } else {
        ApiClient client = std::move(*clientOpt);
        const std::string base = Session::instance().baseUrl();
        const HttpResponse resp = fetchJson(
            client,
            url::buildUrl(base, "/v1/artist",
                          {{"id", args.id}, {"platform", args.platform}}));
        if (!isHttpOk(resp)) {
            msg = "无法获取歌手信息";
        } else {
            auto content = parseArtistDetailContent(
                parseJson(resp.body), args.id, args.platform, args.title);
            info = std::move(content.info);
            embeddedSongs = std::move(content.songs);
            st = Status::Loaded;
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        bannerStatus_ = st;
        bannerMessage_ = std::move(msg);
        info_ = std::move(info);
        // Seed the Songs tab with the /v1/artist embedded list ONLY when the
        // dedicated /v1/artist/songs worker hasn't decided yet. If it has
        // already committed (Loaded with empty result, or Error), leaving
        // its canonical answer in place is correct -- bannerStatus_ resolving
        // later must not paper over a deliberate empty / failure (Codex R2).
        if (st == Status::Loaded && songsState_.status == Status::Loading &&
            songs_.empty() && !embeddedSongs.empty()) {
            songs_ = std::move(embeddedSongs);
        }
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

void ArtistDetailPage::songsWorker(WorkerArgs args, std::uint64_t myGen) {
    HttpClient http;
    auto clientOpt = buildSessionClient(http);
    if (!clientOpt) {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        songsState_.status = Status::Error;
        songsState_.message = "会话未初始化";
        PostMessageW(host_, kDoneMessage, 0, 0);
        return;
    }
    ApiClient client = std::move(*clientOpt);
    const std::string base = Session::instance().baseUrl();
    std::string err;

    for (int pageIdx = 1; pageIdx <= kMaxPages; ++pageIdx) {
        const HttpResponse resp = fetchJson(
            client,
            url::buildUrl(base, "/v1/artist/songs",
                          {{"id", args.id},
                           {"platform", args.platform},
                           {"page_index", std::to_string(pageIdx)},
                           {"page_size", std::to_string(kSongsPageSize)}}));
        if (!isHttpOk(resp)) {
            err = "无法获取歌手歌曲";
            break;
        }
        auto chunk = parseArtistSongsPage(parseJson(resp.body), args.platform);
        const bool hasMore = chunk.hasMore;
        {
            const std::lock_guard<std::mutex> lk(mu_);
            if (myGen != gen_.load()) {
                return;
            }
            // Page 1 replaces whatever the banner seed-and-continue path put
            // in songs_; subsequent pages append. This keeps the canonical
            // ordering from /v1/artist/songs without duplicating the seed.
            if (pageIdx == 1) {
                songs_ = std::move(chunk.items);
            } else {
                for (auto& s : chunk.items) {
                    songs_.push_back(std::move(s));
                }
            }
            songsState_.status = hasMore ? Status::Loading : Status::Loaded;
            songsState_.message.clear();
        }
        PostMessageW(host_, kDoneMessage, 0, 0);
        if (!hasMore) {
            return;
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        if (!err.empty()) {
            // A mid-stream failure leaves songs_ truncated; surface the error
            // so the state isn't a false "Loaded" (Codex review). The
            // already-fetched rows stay visible since drawBodyStatusIfEmpty
            // only paints the status when itemCount == 0.
            songsState_.status = Status::Error;
            songsState_.message = std::move(err);
        } else {
            songsState_.status = Status::Loaded;
        }
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

void ArtistDetailPage::albumsWorker(WorkerArgs args, std::uint64_t myGen) {
    HttpClient http;
    auto clientOpt = buildSessionClient(http);
    if (!clientOpt) {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        albumsState_.status = Status::Error;
        albumsState_.message = "会话未初始化";
        PostMessageW(host_, kDoneMessage, 0, 0);
        return;
    }
    ApiClient client = std::move(*clientOpt);
    const std::string base = Session::instance().baseUrl();
    std::string err;

    for (int pageIdx = 1; pageIdx <= kMaxPages; ++pageIdx) {
        const HttpResponse resp = fetchJson(
            client,
            url::buildUrl(base, "/v1/artist/albums",
                          {{"id", args.id},
                           {"platform", args.platform},
                           {"page_index", std::to_string(pageIdx)},
                           {"page_size", std::to_string(kCardsPageSize)}}));
        if (!isHttpOk(resp)) {
            err = "无法获取歌手专辑";
            break;
        }
        auto chunk = parseArtistAlbumsPage(parseJson(resp.body), args.platform);
        const bool hasMore = chunk.hasMore;
        {
            const std::lock_guard<std::mutex> lk(mu_);
            if (myGen != gen_.load()) {
                return;
            }
            for (auto& a : chunk.items) {
                albums_.push_back(std::move(a));
            }
            albumsState_.status = hasMore ? Status::Loading : Status::Loaded;
            albumsState_.message.clear();
        }
        PostMessageW(host_, kDoneMessage, 0, 0);
        if (!hasMore) {
            return;
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        if (!err.empty()) {
            albumsState_.status = Status::Error;
            albumsState_.message = std::move(err);
        } else {
            albumsState_.status = Status::Loaded;
        }
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

void ArtistDetailPage::mvsWorker(WorkerArgs args, std::uint64_t myGen) {
    HttpClient http;
    auto clientOpt = buildSessionClient(http);
    if (!clientOpt) {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        mvsState_.status = Status::Error;
        mvsState_.message = "会话未初始化";
        PostMessageW(host_, kDoneMessage, 0, 0);
        return;
    }
    ApiClient client = std::move(*clientOpt);
    const std::string base = Session::instance().baseUrl();
    std::string err;

    for (int pageIdx = 1; pageIdx <= kMaxPages; ++pageIdx) {
        const HttpResponse resp = fetchJson(
            client,
            url::buildUrl(base, "/v1/artist/mvs",
                          {{"id", args.id},
                           {"platform", args.platform},
                           {"page_index", std::to_string(pageIdx)},
                           {"page_size", std::to_string(kCardsPageSize)}}));
        if (!isHttpOk(resp)) {
            err = "无法获取歌手 MV";
            break;
        }
        auto chunk = parseArtistMvsPage(parseJson(resp.body), args.platform);
        const bool hasMore = chunk.hasMore;
        {
            const std::lock_guard<std::mutex> lk(mu_);
            if (myGen != gen_.load()) {
                return;
            }
            for (auto& v : chunk.items) {
                mvs_.push_back(std::move(v));
            }
            mvsState_.status = hasMore ? Status::Loading : Status::Loaded;
            mvsState_.message.clear();
        }
        PostMessageW(host_, kDoneMessage, 0, 0);
        if (!hasMore) {
            return;
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        if (!err.empty()) {
            mvsState_.status = Status::Error;
            mvsState_.message = std::move(err);
        } else {
            mvsState_.status = Status::Loaded;
        }
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

void ArtistDetailPage::ensureTabLoaded(ArtistTab tab) {
    // mu_ already held by caller. Snapshot args + gen out of the lock-held
    // section before kicking the worker (workers re-take mu_ internally).
    if (!Session::instance().isAuthenticated() || args_.id.empty() ||
        args_.platform.empty()) {
        return;
    }
    ListStatus* state = nullptr;
    switch (tab) {
        case ArtistTab::Songs:
            state = &songsState_;
            break;
        case ArtistTab::Albums:
            state = &albumsState_;
            break;
        case ArtistTab::Mvs:
            state = &mvsState_;
            break;
    }
    if (state == nullptr || state->started) {
        return;
    }
    state->started = true;
    state->status = Status::Loading;
    state->message.clear();
    const WorkerArgs args = args_;
    const std::uint64_t myGen = gen_.load();
    switch (tab) {
        case ArtistTab::Songs:
            workers_.emplace_back(
                [this, args, myGen] { songsWorker(args, myGen); });
            break;
        case ArtistTab::Albums:
            workers_.emplace_back(
                [this, args, myGen] { albumsWorker(args, myGen); });
            break;
        case ArtistTab::Mvs:
            workers_.emplace_back(
                [this, args, myGen] { mvsWorker(args, myGen); });
            break;
    }
}

bool ArtistDetailPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
    if (msg != kDoneMessage && msg != kCoverReadyMessage) {
        return false;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return true;
}

void ArtistDetailPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

bool ArtistDetailPage::hitTab(float xDip, float yDipContent,
                              std::size_t& outIdx) const {
    if (lastWidthDip_ <= 0.0F) {
        return false;
    }
    // Recompute the tab strip rects from the cached metrics + width. The
    // active tab + body item count don't affect banner/tab geometry, so we
    // can use the active tab here.
    std::size_t itemCount = 0;
    {
        // mu_ is held by caller (onLeftDown). Read item count for the current
        // active tab to keep layout numbers consistent with the last paint.
        switch (activeTab_) {
            case ArtistTab::Songs:
                itemCount = songs_.size();
                break;
            case ArtistTab::Albums:
                itemCount = albums_.size();
                break;
            case ArtistTab::Mvs:
                itemCount = mvs_.size();
                break;
        }
    }
    const ArtistDetailLayout layout =
        computeArtistLayout(activeTab_, itemCount, lastWidthDip_, lastMetrics_);
    for (std::size_t i = 0; i < layout.tabBar.tabs.size(); ++i) {
        if (rectContains(layout.tabBar.tabs.at(i), xDip, yDipContent)) {
            outIdx = i;
            return true;
        }
    }
    return false;
}

bool ArtistDetailPage::onLeftDown(float xDip, float yDip) {
    const float yContent = yDip + scrollY_;
    AlbumInfo albumTarget;
    bool albumHit = false;
    bool needRepaint = false;
    {
        const std::lock_guard<std::mutex> lk(mu_);
        // Banner / tab geometry is only valid after the banner has finished
        // loading; before that lastWidthDip_ may be 0 OR the tab strip rects
        // are absent on screen (a centered status overlay covers everything).
        if (lastWidthDip_ <= 0.0F || bannerStatus_ != Status::Loaded) {
            return false;
        }
        std::size_t tabIdx = 0;
        if (hitTab(xDip, yContent, tabIdx)) {
            const auto wanted = static_cast<ArtistTab>(tabIdx);
            if (wanted != activeTab_) {
                activeTab_ = wanted;
                scrollY_ = 0.0F;
                needRepaint = true;
                ensureTabLoaded(wanted);
            }
        } else if (activeTab_ == ArtistTab::Albums && onAlbumOpen_ &&
                   !albums_.empty()) {
            const ArtistDetailLayout layout = computeArtistLayout(
                activeTab_, albums_.size(), lastWidthDip_, lastMetrics_);
            const std::size_t n =
                std::min(layout.body.items.size(), albums_.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (rectContains(layout.body.items.at(i), xDip, yContent)) {
                    albumTarget = albums_.at(i);
                    albumHit = true;
                    break;
                }
            }
        }
    }
    if (albumHit) {
        onAlbumOpen_(albumTarget);
        return true;
    }
    if (needRepaint && host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return needRepaint;
}

namespace {

void drawSongBody(const Renderer& rn, const SectionLayout& body,
                  const std::vector<SongInfo>& songs) {
    const std::size_t n = std::min(body.items.size(), songs.size());
    for (std::size_t i = 0; i < n; ++i) {
        const D2D1_RECT_F s = rn.screen(body.items.at(i));
        if (!rn.visible(s)) {
            continue;
        }
        rn.songRow(s, static_cast<int>(i) + 1, utf8ToWide(songs.at(i).name),
                   utf8ToWide(songArtistText(songs.at(i))), songs.at(i).cover);
    }
}

template <class T, class TitleFn, class SubFn, class CoverFn>
void drawCardBody(const Renderer& rn, const SectionLayout& body,
                  const std::vector<T>& items, bool square, TitleFn titleFn,
                  SubFn subFn, CoverFn coverFn) {
    const std::size_t n = std::min(body.items.size(), items.size());
    for (std::size_t i = 0; i < n; ++i) {
        const D2D1_RECT_F s = rn.screen(body.items.at(i));
        if (!rn.visible(s)) {
            continue;
        }
        rn.card(s, square, utf8ToWide(titleFn(items.at(i))),
                utf8ToWide(subFn(items.at(i))), coverFn(items.at(i)));
    }
}

void drawBodyPlaceholder(ID2D1RenderTarget* rt, const Renderer& rn,
                         const ArtistTabBarLayout& bar, float sectionGap,
                         float rowHeight, const std::wstring& text) {
    const D2D1_RECT_F band =
        rn.screen({bar.band.left, bar.band.bottom + sectionGap, bar.band.right,
                   bar.band.bottom + sectionGap + rowHeight});
    if (rn.visible(band)) {
        drawText(rt, rn.subFmt.Get(), text, rn.subBrush.Get(), band);
    }
}

}  // namespace

bool ArtistDetailPage::drawBannerStatusOverlay(ID2D1RenderTarget* rt,
                                               const Theme& theme,
                                               D2D1_SIZE_F size) const {
    switch (bannerStatus_) {
        case Status::Idle:
            drawCentered(rt, theme, size, L"正在准备…");
            return true;
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在加载歌手信息…");
            return true;
        case Status::NotLoggedIn:
            drawCentered(rt, theme, size,
                         L"请先登录 HE-Music（文件 > HE-Music > Login）");
            return true;
        case Status::Error:
            drawCentered(rt, theme, size,
                         utf8ToWide(bannerMessage_.empty() ? "加载失败"
                                                           : bannerMessage_));
            return true;
        case Status::Loaded:
            return false;
    }
    return false;
}

std::size_t ArtistDetailPage::activeItemCount() const {
    switch (activeTab_) {
        case ArtistTab::Songs:
            return songs_.size();
        case ArtistTab::Albums:
            return albums_.size();
        case ArtistTab::Mvs:
            return mvs_.size();
    }
    return 0;
}

const ArtistDetailPage::ListStatus& ArtistDetailPage::activeListState() const {
    switch (activeTab_) {
        case ArtistTab::Albums:
            return albumsState_;
        case ArtistTab::Mvs:
            return mvsState_;
        case ArtistTab::Songs:
        default:
            return songsState_;
    }
}

bool ArtistDetailPage::drawBodyStatusIfEmpty(ID2D1RenderTarget* rt,
                                             const Renderer& rn,
                                             const ArtistTabBarLayout& bar,
                                             const LayoutMetrics& m,
                                             std::size_t itemCount) const {
    if (itemCount > 0) {
        return false;
    }
    const ListStatus& state = activeListState();
    if (state.status == Status::Loading) {
        drawBodyPlaceholder(rt, rn, bar, m.sectionGap, m.rowHeight,
                            L"正在加载…");
        return true;
    }
    if (state.status == Status::Error) {
        drawBodyPlaceholder(
            rt, rn, bar, m.sectionGap, m.rowHeight,
            utf8ToWide(state.message.empty() ? "加载失败" : state.message));
        return true;
    }
    if (state.status == Status::Loaded) {
        const wchar_t* empty = activeTab_ == ArtistTab::Songs    ? L"暂无歌曲"
                               : activeTab_ == ArtistTab::Albums ? L"暂无专辑"
                                                                 : L"暂无 MV";
        drawBodyPlaceholder(rt, rn, bar, m.sectionGap, m.rowHeight, empty);
        return true;
    }
    return false;
}

void ArtistDetailPage::drawActiveBody(const Renderer& rn,
                                      const SectionLayout& body) const {
    switch (activeTab_) {
        case ArtistTab::Songs:
            drawSongBody(rn, body, songs_);
            break;
        case ArtistTab::Albums:
            drawCardBody(
                rn, body, albums_, /*square=*/true,
                [](const AlbumInfo& a) { return a.name; },
                [](const AlbumInfo& a) { return artistNamesText(a.artists); },
                [](const AlbumInfo& a) { return a.cover; });
            break;
        case ArtistTab::Mvs:
            drawCardBody(
                rn, body, mvs_, /*square=*/false,
                [](const MvInfo& v) { return v.name; },
                [](const MvInfo& v) { return v.creator; },
                [](const MvInfo& v) { return v.cover; });
            break;
    }
}

void ArtistDetailPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                             D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);
    lastWidthDip_ = size.width;
    if (drawBannerStatusOverlay(rt, theme, size)) {
        return;
    }

    const LayoutMetrics m = currentMetrics(theme);
    lastMetrics_ = m;

    const std::size_t itemCount = activeItemCount();
    const ArtistDetailLayout layout =
        computeArtistLayout(activeTab_, itemCount, size.width, m);
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

    ComPtr<ID2D1SolidColorBrush> selBrush;
    rt->CreateSolidColorBrush(theme.selection, selBrush.GetAddressOf());
    ComPtr<IDWriteTextFormat> bannerTitleFmt =
        makeFormat(theme.fontFamily, theme.sectionTitleSize * kBannerTitleScale,
                   /*centered=*/false, /*ellipsis=*/true);
    ComPtr<IDWriteTextFormat> tabFmt =
        makeFormat(theme.fontFamily, theme.rowTitleSize,
                   /*centered=*/true, /*ellipsis=*/false);

    // --- Banner ---
    const D2D1_RECT_F coverS = rn.screen(layout.bannerCover);
    if (rn.visible(coverS)) {
        rn.drawCover(coverS, info_.cover);
    }
    const D2D1_RECT_F infoS = rn.screen(layout.bannerInfo);
    if (rn.visible(infoS)) {
        paintBannerInfo(rt, theme, rn, bannerTitleFmt.Get(), infoS, info_);
    }

    // --- Tab strip + active body ---
    paintTabStrip(rt, theme, rn, tabFmt.Get(), layout.tabBar, selBrush.Get(),
                  activeTab_);
    if (drawBodyStatusIfEmpty(rt, rn, layout.tabBar, m, itemCount)) {
        return;
    }
    drawActiveBody(rn, layout.body);
}

}  // namespace hemusic::ui
