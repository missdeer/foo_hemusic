#include "ui/pages/radio_detail_page.h"

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

#include "api/radio.h"
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

LayoutMetrics currentMetrics(const Theme& theme) {
    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;
    return m;
}

}  // namespace

RadioDetailPage::~RadioDetailPage() {
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

void RadioDetailPage::enter(const nav::PageEntry& entry) {
    const auto pickParam = [&](const char* key) -> std::string {
        auto it = entry.params.find(key);
        return it == entry.params.end() ? std::string{} : it->second;
    };
    WorkerArgs args{pickParam("id"), pickParam("platform"), pickParam("title"),
                    pickParam("cover")};
    if (args.title.empty()) {
        args.title = entry.title;
    }

    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;

    const std::uint64_t myGen = gen_.fetch_add(1) + 1;

    if (args.id.empty() || args.platform.empty()) {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Error;
        message_ = "电台参数缺失";
        title_ = args.title;
        cover_ = args.cover;
        songs_.clear();
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    if (!Session::instance().isAuthenticated()) {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::NotLoggedIn;
        title_ = args.title;
        cover_ = args.cover;
        songs_.clear();
        message_.clear();
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Loading;
        title_ = args.title;
        cover_ = args.cover;
        songs_.clear();
        message_.clear();
    }
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    workers_.emplace_back([this, args, myGen] { worker(args, myGen); });
}

void RadioDetailPage::reset() {
    gen_.fetch_add(1);
    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Idle;
        title_.clear();
        cover_.clear();
        songs_.clear();
        message_.clear();
    }
    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void RadioDetailPage::worker(WorkerArgs args, std::uint64_t myGen) {
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
        HttpRequest req;
        req.url = url::buildUrl(session.baseUrl(), "/v1/radio/songs",
                                {{"id", args.id}, {"platform", args.platform}});
        req.connectTimeoutMs = kFetchConnectMs;
        req.readTimeoutMs = kFetchReadMs;
        const HttpResponse resp = client.send(req);
        if (!isHttpOk(resp)) {
            msg = "无法获取电台歌曲";
        } else {
            songs = parseRadioSongs(parseJson(resp.body), args.platform);
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
        songs_ = std::move(songs);
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool RadioDetailPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
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

void RadioDetailPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void RadioDetailPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                            D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);

    switch (status_) {
        case Status::Idle:
            drawCentered(rt, theme, size, L"正在准备…");
            return;
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在加载电台歌曲…");
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
    const DetailLayout layout =
        computeDetailLayout(songs_.size(), size.width, m);
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

    ComPtr<IDWriteTextFormat> bannerTitleFmt =
        makeFormat(theme.fontFamily, theme.sectionTitleSize * kBannerTitleScale,
                   /*centered=*/false, /*ellipsis=*/true);

    // --- Banner cover ---
    const D2D1_RECT_F coverS = rn.screen(layout.bannerCover);
    if (rn.visible(coverS)) {
        rn.drawCover(coverS, cover_);
    }
    // --- Banner info: title + song count ---
    const D2D1_RECT_F infoS = rn.screen(layout.bannerInfo);
    if (rn.visible(infoS)) {
        const float titleH =
            theme.sectionTitleSize * kBannerTitleScale + kBannerSubGap;
        const float subLineH = theme.rowSubSize + kBannerSubGap;
        float y = infoS.top + kBannerSubGap;
        drawText(rt, bannerTitleFmt.Get(), utf8ToWide(title_),
                 rn.textBrush.Get(),
                 D2D1::RectF(infoS.left, y, infoS.right, y + titleH));
        y += titleH;
        const std::string counts = std::to_string(songs_.size()) + " 首";
        drawText(rt, rn.subFmt.Get(), utf8ToWide(counts), rn.subBrush.Get(),
                 D2D1::RectF(infoS.left, y, infoS.right, y + subLineH));
    }
    // (no enqueue button on radio detail -- HEMUSIC-5 is deferred and radio
    // playback streams continuously anyway; no clean "queue all" semantics.)

    // --- Songs list ---
    drawSongListSection(
        rn, layout.songs, songs_, L"曲目",
        [](const SongInfo& s) { return s.name; },
        [](const SongInfo& s) { return songArtistText(s); },
        [](const SongInfo& s) { return s.cover; });
}

}  // namespace hemusic::ui
