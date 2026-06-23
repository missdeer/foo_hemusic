#include "ui/pages/playlist_detail_page.h"

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
#include <vector>

#include "api/playlist.h"
#include "api/playlist_detail.h"
#include "api/song.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/cover_cache.h"
#include "ui/image_cache.h"
#include "ui/pages/playlist_detail_layout.h"
#include "ui/pages/section_render.h"

// No SDK headers: Session / ApiClient / HttpClient are SDK-free; drawing is
// plain Win32 + Direct2D shared via section_render.h.

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;
using render_detail::isHttpOk;
using render_detail::kFetchConnectMs;
using render_detail::kFetchReadMs;
using render_detail::parseJson;

constexpr float kScrollStepPx = 120.0F;
constexpr float kTitleBandFactor = 1.6F;
// Banner title font multiplier (NIT N3) -- larger than section titles so the
// playlist name dominates the banner.
constexpr float kBannerTitleScale = 1.25F;
constexpr float kBannerSubGap = 4.0F;  // gap between banner text lines
constexpr float kButtonRadius = 4.0F;  // enqueue button rounded corner
// Press tint: blend the button background `kPressBlend` of the way toward text.
constexpr float kPressBlend = 0.20F;
// Hover tint: solid fill at `kHoverAlpha` of the selection color.
constexpr float kHoverAlpha = 0.85F;

// Page-local layout metrics. The shared LayoutMetrics defaults carry both the
// list/grid constants and our `detail*` banner sizes; we only override what the
// theme dictates (padding / rowHeight / titleBand follow whatever the theme
// sets so paint + hit-test stay aligned -- see currentMetrics()).
LayoutMetrics currentMetrics(const Theme& theme) {
    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;
    return m;
}

// COLORREF-free linear blend between two D2D colors.
D2D1_COLOR_F lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

bool rectContains(const LayoutRect& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

D2D1_RECT_F toD2D(const LayoutRect& r) {
    return D2D1::RectF(r.left, r.top, r.right, r.bottom);
}

// Concatenates "<song_count> 首 · 播放 <play_count>" from whichever fields the
// payload carries; empty when both are absent.
std::string formatCounts(const PlaylistInfo& info) {
    std::string out;
    if (!info.songCount.empty()) {
        out = info.songCount + " 首";
    }
    if (!info.playCount.empty()) {
        if (!out.empty()) {
            out += " · ";
        }
        out += "播放 " + info.playCount;
    }
    return out;
}

// Paints the banner info column: title (big), creator, counts. Caller must
// hold PlaylistDetailPage::mu_.
void paintBannerInfo(ID2D1RenderTarget* rt, const Theme& theme,
                     const Renderer& rn, IDWriteTextFormat* bannerTitleFmt,
                     const D2D1_RECT_F& infoS, float btnBandH,
                     const PlaylistInfo& info) {
    const float titleH =
        theme.sectionTitleSize * kBannerTitleScale + kBannerSubGap;
    const float subLineH = theme.rowSubSize + kBannerSubGap;
    float y = infoS.top + kBannerSubGap;

    drawText(rt, bannerTitleFmt, utf8ToWide(info.name), rn.textBrush.Get(),
             D2D1::RectF(infoS.left, y, infoS.right, y + titleH));
    y += titleH;

    const std::string creator = "创建者：" + info.creator;
    drawText(rt, rn.subFmt.Get(), utf8ToWide(creator), rn.subBrush.Get(),
             D2D1::RectF(infoS.left, y, infoS.right, y + subLineH));
    y += subLineH;

    const std::string counts = formatCounts(info);
    if (counts.empty()) {
        return;
    }
    const float bottom = std::min(infoS.bottom - btnBandH, y + subLineH);
    drawText(rt, rn.subFmt.Get(), utf8ToWide(counts), rn.subBrush.Get(),
             D2D1::RectF(infoS.left, y, infoS.right, bottom));
}

// Picks the button background brush per (hover, pressing) state. Returns null
// for the idle / outline-only case (no fill).
Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> makeButtonFill(
    ID2D1RenderTarget* rt, const Theme& theme, bool hover, bool pressing) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
    if (pressing && hover) {
        rt->CreateSolidColorBrush(
            lerp(theme.selection, theme.text, kPressBlend), b.GetAddressOf());
    } else if (hover) {
        D2D1_COLOR_F hoverColor = theme.selection;
        hoverColor.a = kHoverAlpha;
        rt->CreateSolidColorBrush(hoverColor, b.GetAddressOf());
    }
    return b;
}

// Self-drawn enqueue stub: fill (hover/press) + outline + label. selBrush must
// be non-null; visibility check + ★★ S5 stub semantics handled by caller.
void paintEnqueueButton(ID2D1RenderTarget* rt, const Theme& theme,
                        IDWriteTextFormat* btnFmt, const D2D1_RECT_F& btnS,
                        ID2D1SolidColorBrush* selBrush, bool hover,
                        bool pressing) {
    const D2D1_ROUNDED_RECT rr{btnS, kButtonRadius, kButtonRadius};
    auto fill = makeButtonFill(rt, theme, hover, pressing);
    if (fill) {
        rt->FillRoundedRectangle(rr, fill.Get());
    }
    rt->DrawRoundedRectangle(rr, selBrush, render_detail::kStrokeWidth);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> labelBrush;
    const D2D1_COLOR_F labelColor = hover ? theme.background : theme.selection;
    rt->CreateSolidColorBrush(labelColor, labelBrush.GetAddressOf());
    if (labelBrush) {
        drawText(rt, btnFmt, L"全部入列", labelBrush.Get(), btnS);
    }
}

}  // namespace

PlaylistDetailPage::~PlaylistDetailPage() {
    // Bump gen so any worker that wakes up after we start tearing down will
    // discard its result; then join every worker still on file. Each worker is
    // bounded by (connect 8s + read 15s) * 2 ≈ 46s worst case (network dead)
    // but completes far faster in practice.
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

void PlaylistDetailPage::joinFinishedWorkers() {
    // ★ R1-M2: must NOT block the UI thread on still-running workers. There's
    // no portable lock-free "thread done?" probe, so we just leave joinable
    // threads in place; they accumulate for the life of the page (one entry
    // per playlist opened) but each holds only a small thread handle and is
    // joined in the destructor once it has run to completion (gen check inside
    // mu_ guarantees its writes are discarded). Net cost: a few dozen bytes
    // per opened playlist for one panel lifetime -- negligible vs the freeze.
}

void PlaylistDetailPage::enter(const nav::PageEntry& entry) {
    const auto pickParam = [&](const char* key) -> std::string {
        auto it = entry.params.find(key);
        return it == entry.params.end() ? std::string{} : it->second;
    };
    WorkerArgs args{pickParam("id"), pickParam("platform"), pickParam("title")};
    if (args.title.empty()) {
        args.title = entry.title;
    }

    // UI-thread: reset scroll + button state for the new page.
    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    btnHover_ = false;
    btnPressing_ = false;

    // Sweep finished workers (best-effort; superseded ones will drop their
    // results without writing).
    joinFinishedWorkers();

    const std::uint64_t myGen = gen_.fetch_add(1) + 1;

    if (args.id.empty() || args.platform.empty()) {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Error;
        message_ = "歌单参数缺失";
        info_ = PlaylistInfo{};
        info_.name = args.title;
        songs_.clear();
        activeId_ = args.id;
        activePlatform_ = args.platform;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    if (!Session::instance().isAuthenticated()) {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::NotLoggedIn;
        info_ = PlaylistInfo{};
        info_.name = args.title;
        info_.id = args.id;
        info_.platform = args.platform;
        songs_.clear();
        message_.clear();
        activeId_ = args.id;
        activePlatform_ = args.platform;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Loading;
        info_ = PlaylistInfo{};
        info_.name = args.title;
        info_.id = args.id;
        info_.platform = args.platform;
        songs_.clear();
        message_.clear();
        activeId_ = args.id;
        activePlatform_ = args.platform;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    workers_.emplace_back([this, args, myGen] { worker(args, myGen); });
}

void PlaylistDetailPage::reset() {
    // Bump gen so all in-flight workers drop their results.
    gen_.fetch_add(1);
    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Idle;
        info_ = PlaylistInfo{};
        songs_.clear();
        message_.clear();
        activeId_.clear();
        activePlatform_.clear();
    }
    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    btnHover_ = false;
    btnPressing_ = false;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void PlaylistDetailPage::worker(WorkerArgs args, std::uint64_t myGen) {
    HttpClient http;
    ApiClient::Transport transport = [&http](const HttpRequest& r) {
        return http.send(r);
    };

    Status st = Status::Error;
    std::string msg;
    PlaylistInfo info;
    info.id = args.id;
    info.platform = args.platform;
    info.name = args.title;
    std::vector<SongInfo> songs;

    Session& session = Session::instance();
    auto clientOpt = session.buildClient(transport);
    if (!clientOpt) {
        msg = "会话未初始化";
    } else {
        ApiClient client = std::move(*clientOpt);
        const std::string base = session.baseUrl();

        HttpRequest detailReq;
        detailReq.url =
            url::buildUrl(base, "/v1/playlist",
                          {{"id", args.id}, {"platform", args.platform}});
        detailReq.connectTimeoutMs = kFetchConnectMs;
        detailReq.readTimeoutMs = kFetchReadMs;
        const HttpResponse detailResp = client.send(detailReq);
        if (!isHttpOk(detailResp)) {
            msg = "无法获取歌单信息";
        } else {
            info = parsePlaylistDetailInfo(parseJson(detailResp.body), args.id,
                                           args.platform, args.title);
            HttpRequest songsReq;
            songsReq.url = url::buildUrl(base, "/v1/playlist/songs",
                                         {{"id", args.id},
                                          {"platform", args.platform},
                                          {"page_index", "1"},
                                          {"page_size", "1000"}});
            songsReq.connectTimeoutMs = kFetchConnectMs;
            songsReq.readTimeoutMs = kFetchReadMs;
            const HttpResponse songsResp = client.send(songsReq);
            if (!isHttpOk(songsResp)) {
                msg = "无法获取歌曲列表";
            } else {
                songs = parsePlaylistSongs(parseJson(songsResp.body),
                                           args.platform);
                st = Status::Loaded;
            }
        }
    }

    // ★★ M6 TOCTOU: gen check is INSIDE the lock so we can't be preempted
    // between read-gen and write-state by a newer worker.
    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;  // superseded -- drop the result silently
        }
        status_ = st;
        message_ = std::move(msg);
        info_ = std::move(info);
        songs_ = std::move(songs);
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool PlaylistDetailPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
    if (msg == kDoneMessage) {
        scrollY_ = 0.0F;  // fresh data -> back to top
    } else if (msg != kCoverReadyMessage) {
        return false;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return true;
}

void PlaylistDetailPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

bool PlaylistDetailPage::hitEnqueueButton(float xDip, float yDipContent) const {
    // Need both: the latest paint width (for layout) and Loaded status (only
    // then is the button meaningful). Caller has already converted yDip ->
    // content coords (yDip + scrollY_).
    if (lastWidthDip_ <= 0.0F) {
        return false;
    }
    LayoutMetrics m;
    {
        // We don't actually need mu_ for status_ here since the read is
        // best-effort and a stale value just causes one extra repaint, but
        // staying consistent with M7 keeps the rule simple.
        const std::lock_guard<std::mutex> lk(mu_);
        if (status_ != Status::Loaded) {
            return false;
        }
    }
    // ★ M4: hit-test must use the same metrics paint cached, not the defaults
    // (theme.padding could differ in theory; keep paint/hit-test fused via
    // lastMetrics_).
    const PlaylistDetailLayout layout =
        computeDetailLayout(0, lastWidthDip_, lastMetrics_);
    return rectContains(layout.enqueueButton, xDip, yDipContent);
}

bool PlaylistDetailPage::onLeftDown(float xDip, float yDip) {
    const float yContent = yDip + scrollY_;
    if (hitEnqueueButton(xDip, yContent)) {
        btnPressing_ = true;
        btnHover_ = true;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return true;  // request SetCapture from host
    }
    return false;
}

bool PlaylistDetailPage::onMouseMove(float xDip, float yDip) {
    const float yContent = yDip + scrollY_;
    const bool over = hitEnqueueButton(xDip, yContent);
    if (over != btnHover_) {
        btnHover_ = over;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
    }
    return btnPressing_;
}

bool PlaylistDetailPage::onLeftUp(float xDip, float yDip) {
    const bool wasPressing = btnPressing_;
    btnPressing_ = false;
    const float yContent = yDip + scrollY_;
    const bool over = hitEnqueueButton(xDip, yContent);
    btnHover_ = over;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    if (wasPressing && over && onEnqueueAll_) {
        // ★★ M7: copy out under mu_, then invoke callback unlocked so the
        // callback can't deadlock (or starve worker writes) by re-entering.
        PlaylistInfo infoCopy;
        std::vector<SongInfo> songsCopy;
        {
            const std::lock_guard<std::mutex> lk(mu_);
            infoCopy = info_;
            songsCopy = songs_;
        }
        onEnqueueAll_(infoCopy, songsCopy);
    }
    return wasPressing;
}

void PlaylistDetailPage::onMouseLeave() {
    if (btnHover_) {
        btnHover_ = false;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
    }
}

void PlaylistDetailPage::onCaptureLost() {
    if (btnPressing_) {
        btnPressing_ = false;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
    }
}

void PlaylistDetailPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                               D2D1_SIZE_F size) {
    // ★★ M7: every UI-thread read of info_ / songs_ / status_ / message_
    // happens under mu_. We hold mu_ for the whole paint so the worker can't
    // commit a fresh result mid-draw.
    const std::lock_guard<std::mutex> lk(mu_);
    lastWidthDip_ = size.width;

    switch (status_) {
        case Status::Idle:
            drawCentered(rt, theme, size, L"正在准备…");
            return;
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在加载歌单详情…");
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

    const LayoutMetrics m = currentMetrics(theme);
    lastMetrics_ = m;  // ★ M4: paint metrics cached for onLeftDown hit-test
    const PlaylistDetailLayout layout =
        computeDetailLayout(songs_.size(), size.width, m);
    contentHeight_ = layout.contentHeight;
    const float maxScroll = std::max(0.0F, contentHeight_ - size.height);
    scrollY_ = std::clamp(scrollY_, 0.0F, maxScroll);

    // --- Build shared renderer (also used for the song-list section) ---
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

    // Auxiliary brush for the enqueue button fills + a banner title format
    // distinct from the standard section title (NIT N3: 1.25x).
    ComPtr<ID2D1SolidColorBrush> selBrush;
    rt->CreateSolidColorBrush(theme.selection, selBrush.GetAddressOf());
    ComPtr<IDWriteTextFormat> bannerTitleFmt =
        makeFormat(theme.fontFamily, theme.sectionTitleSize * kBannerTitleScale,
                   /*centered=*/false, /*ellipsis=*/true);
    ComPtr<IDWriteTextFormat> btnFmt =
        makeFormat(theme.fontFamily, theme.rowTitleSize,
                   /*centered=*/true, /*ellipsis=*/false);

    // --- Banner ---
    const D2D1_RECT_F coverS = rn.screen(layout.bannerCover);
    if (rn.visible(coverS)) {
        rn.drawCover(coverS, info_.cover);
    }
    const D2D1_RECT_F infoS = rn.screen(layout.bannerInfo);
    const float btnH = layout.enqueueButton.bottom - layout.enqueueButton.top;
    if (rn.visible(infoS)) {
        paintBannerInfo(rt, theme, rn, bannerTitleFmt.Get(), infoS, btnH,
                        info_);
    }

    const D2D1_RECT_F btnS = rn.screen(layout.enqueueButton);
    if (rn.visible(btnS) && selBrush) {
        paintEnqueueButton(rt, theme, btnFmt.Get(), btnS, selBrush.Get(),
                           btnHover_, btnPressing_);
    }

    // --- Songs section ---
    drawSongListSection(
        rn, layout.songs, songs_, L"歌曲列表",
        [](const SongInfo& s) { return s.name; },
        [](const SongInfo& s) { return songArtistText(s); },
        [](const SongInfo& s) { return s.cover; });
}

}  // namespace hemusic::ui
