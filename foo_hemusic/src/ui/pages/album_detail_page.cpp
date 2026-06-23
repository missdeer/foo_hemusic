#include "ui/pages/album_detail_page.h"

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

#include "api/album.h"
#include "api/album_detail.h"
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
constexpr float kButtonRadius = 4.0F;
constexpr float kPressBlend = 0.20F;
constexpr float kHoverAlpha = 0.85F;

LayoutMetrics currentMetrics(const Theme& theme) {
    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;
    return m;
}

D2D1_COLOR_F lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

bool rectContains(const LayoutRect& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// "<artists> · <publish_time>" — drops either side when absent. Used as the
// banner subtitle line, mirroring the y.wjhe.top album page hierarchy where
// the artist name + release year sit immediately under the album title.
std::string formatArtistsAndYear(const AlbumInfo& a) {
    std::string out = artistNamesText(a.artists);  // "-" when no artists
    if (out == "-") {
        out.clear();
    }
    if (!a.publishTime.empty()) {
        if (!out.empty()) {
            out += " · ";
        }
        out += a.publishTime;
    }
    return out;
}

// "<song_count> 首 · 播放 <play_count>" — matches PlaylistDetail's third line.
// Empty when both counts are absent.
std::string formatCounts(const AlbumInfo& a) {
    std::string out;
    if (!a.songCount.empty()) {
        out = a.songCount + " 首";
    }
    if (!a.playCount.empty()) {
        if (!out.empty()) {
            out += " · ";
        }
        out += "播放 " + a.playCount;
    }
    return out;
}

void paintBannerInfo(ID2D1RenderTarget* rt, const Theme& theme,
                     const Renderer& rn, IDWriteTextFormat* bannerTitleFmt,
                     const D2D1_RECT_F& infoS, float btnBandH,
                     const AlbumInfo& a) {
    const float titleH =
        theme.sectionTitleSize * kBannerTitleScale + kBannerSubGap;
    const float subLineH = theme.rowSubSize + kBannerSubGap;
    float y = infoS.top + kBannerSubGap;

    drawText(rt, bannerTitleFmt, utf8ToWide(a.name), rn.textBrush.Get(),
             D2D1::RectF(infoS.left, y, infoS.right, y + titleH));
    y += titleH;

    const std::string artistLine = formatArtistsAndYear(a);
    if (!artistLine.empty()) {
        drawText(rt, rn.subFmt.Get(), utf8ToWide(artistLine), rn.subBrush.Get(),
                 D2D1::RectF(infoS.left, y, infoS.right, y + subLineH));
        y += subLineH;
    }

    const std::string counts = formatCounts(a);
    if (counts.empty()) {
        return;
    }
    const float bottom = std::min(infoS.bottom - btnBandH, y + subLineH);
    drawText(rt, rn.subFmt.Get(), utf8ToWide(counts), rn.subBrush.Get(),
             D2D1::RectF(infoS.left, y, infoS.right, bottom));
}

ComPtr<ID2D1SolidColorBrush> makeButtonFill(ID2D1RenderTarget* rt,
                                            const Theme& theme, bool hover,
                                            bool pressing) {
    ComPtr<ID2D1SolidColorBrush> b;
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

    ComPtr<ID2D1SolidColorBrush> labelBrush;
    const D2D1_COLOR_F labelColor = hover ? theme.background : theme.selection;
    rt->CreateSolidColorBrush(labelColor, labelBrush.GetAddressOf());
    if (labelBrush) {
        drawText(rt, btnFmt, L"全部入列", labelBrush.Get(), btnS);
    }
}

}  // namespace

AlbumDetailPage::~AlbumDetailPage() {
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

void AlbumDetailPage::enter(const nav::PageEntry& entry) {
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
    btnHover_ = false;
    btnPressing_ = false;

    const std::uint64_t myGen = gen_.fetch_add(1) + 1;

    if (args.id.empty() || args.platform.empty()) {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Error;
        message_ = "专辑参数缺失";
        info_ = AlbumInfo{};
        info_.name = args.title;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    if (!Session::instance().isAuthenticated()) {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::NotLoggedIn;
        info_ = AlbumInfo{};
        info_.name = args.title;
        info_.id = args.id;
        info_.platform = args.platform;
        message_.clear();
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Loading;
        info_ = AlbumInfo{};
        info_.name = args.title;
        info_.id = args.id;
        info_.platform = args.platform;
        message_.clear();
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    workers_.emplace_back([this, args, myGen] { worker(args, myGen); });
}

void AlbumDetailPage::reset() {
    gen_.fetch_add(1);
    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Idle;
        info_ = AlbumInfo{};
        message_.clear();
    }
    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    btnHover_ = false;
    btnPressing_ = false;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void AlbumDetailPage::worker(WorkerArgs args, std::uint64_t myGen) {
    HttpClient http;
    ApiClient::Transport transport = [&http](const HttpRequest& r) {
        return http.send(r);
    };

    Status st = Status::Error;
    std::string msg;
    AlbumInfo info;
    info.id = args.id;
    info.platform = args.platform;
    info.name = args.title;

    Session& session = Session::instance();
    auto clientOpt = session.buildClient(transport);
    if (!clientOpt) {
        msg = "会话未初始化";
    } else {
        ApiClient client = std::move(*clientOpt);
        const std::string base = session.baseUrl();

        HttpRequest req;
        req.url = url::buildUrl(base, "/v1/album",
                                {{"id", args.id}, {"platform", args.platform}});
        req.connectTimeoutMs = kFetchConnectMs;
        req.readTimeoutMs = kFetchReadMs;
        const HttpResponse resp = client.send(req);
        if (!isHttpOk(resp)) {
            msg = "无法获取专辑详情";
        } else {
            info = parseAlbumDetailInfo(parseJson(resp.body), args.id,
                                        args.platform, args.title);
            st = Status::Loaded;
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (myGen != gen_.load()) {
            return;
        }
        status_ = st;
        message_ = std::move(msg);
        info_ = std::move(info);
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool AlbumDetailPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
    if (msg == kDoneMessage) {
        scrollY_ = 0.0F;
    } else if (msg != kCoverReadyMessage) {
        return false;
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    return true;
}

void AlbumDetailPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

bool AlbumDetailPage::hitEnqueueButton(float xDip, float yDipContent) const {
    if (lastWidthDip_ <= 0.0F) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (status_ != Status::Loaded) {
            return false;
        }
    }
    const DetailLayout layout =
        computeDetailLayout(0, lastWidthDip_, lastMetrics_);
    return rectContains(layout.enqueueButton, xDip, yDipContent);
}

bool AlbumDetailPage::onLeftDown(float xDip, float yDip) {
    const float yContent = yDip + scrollY_;
    if (hitEnqueueButton(xDip, yContent)) {
        btnPressing_ = true;
        btnHover_ = true;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return true;
    }
    return false;
}

bool AlbumDetailPage::onMouseMove(float xDip, float yDip) {
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

bool AlbumDetailPage::onLeftUp(float xDip, float yDip) {
    const bool wasPressing = btnPressing_;
    btnPressing_ = false;
    const float yContent = yDip + scrollY_;
    const bool over = hitEnqueueButton(xDip, yContent);
    btnHover_ = over;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    if (wasPressing && over && onEnqueueAll_) {
        AlbumInfo infoCopy;
        {
            const std::lock_guard<std::mutex> lk(mu_);
            infoCopy = info_;
        }
        onEnqueueAll_(infoCopy);
    }
    return wasPressing;
}

void AlbumDetailPage::onMouseLeave() {
    if (btnHover_) {
        btnHover_ = false;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
    }
}

void AlbumDetailPage::onCaptureLost() {
    if (btnPressing_) {
        btnPressing_ = false;
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
    }
}

void AlbumDetailPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                            D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);
    lastWidthDip_ = size.width;

    switch (status_) {
        case Status::Idle:
            drawCentered(rt, theme, size, L"正在准备…");
            return;
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在加载专辑详情…");
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
    lastMetrics_ = m;
    const DetailLayout layout =
        computeDetailLayout(info_.songs.size(), size.width, m);
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
    ComPtr<IDWriteTextFormat> btnFmt =
        makeFormat(theme.fontFamily, theme.rowTitleSize,
                   /*centered=*/true, /*ellipsis=*/false);

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

    drawSongListSection(
        rn, layout.songs, info_.songs, L"歌曲列表",
        [](const SongInfo& s) { return s.name; },
        [](const SongInfo& s) { return songArtistText(s); },
        [](const SongInfo& s) { return s.cover; });
}

}  // namespace hemusic::ui
