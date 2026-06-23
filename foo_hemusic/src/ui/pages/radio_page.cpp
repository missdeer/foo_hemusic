#include "ui/pages/radio_page.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "api/platforms.h"
#include "api/radio.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/cover_cache.h"
#include "ui/image_cache.h"
#include "ui/pages/discover_layout.h"
#include "ui/pages/radio_layout.h"
#include "ui/pages/section_render.h"

namespace hemusic::ui {

namespace {

using render_detail::isHttpOk;
using render_detail::kFetchConnectMs;
using render_detail::kFetchReadMs;
using render_detail::parseJson;

constexpr float kScrollStepPx = 120.0F;
constexpr float kTitleBandFactor = 1.6F;

LayoutMetrics currentMetrics(const Theme& theme) {
    LayoutMetrics m;
    m.padding = theme.padding;
    m.rowHeight = theme.rowHeight;
    m.titleBand = theme.sectionTitleSize * kTitleBandFactor;
    return m;
}

std::vector<std::size_t> groupSizes(const std::vector<RadioGroupInfo>& groups) {
    std::vector<std::size_t> out;
    out.reserve(groups.size());
    for (const auto& g : groups) {
        out.push_back(g.radios.size());
    }
    return out;
}

}  // namespace

RadioPage::~RadioPage() {
    // Bump gen so anything still running drops its result; then reap all the
    // worker threads we've accumulated. Each one is bounded by the per-request
    // (connect 8s + read 15s) × 2 ≈ 46s worst case (matches
    // PlaylistDetailPage).
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

void RadioPage::load() {
    // ★ R2 MUST-FIX: skip when we already have data or a fetch is in flight.
    // Use status_ (under mu_) as the gate instead of a separate atomic so the
    // gate is consistent with the worker's gen-guarded commit -- the earlier
    // shape used a `loading_` atomic that deadlocked perpetual-Loading on
    // rapid tab toggle (worker overwrote loading_=false on gen mismatch,
    // status_ stayed Loading, next load() saw Loading and bailed forever).
    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (status_ == Status::Loaded || status_ == Status::Loading) {
            return;
        }
    }
    scrollY_ = 0.0F;

    if (!Session::instance().isAuthenticated()) {
        {
            const std::lock_guard<std::mutex> lk(mu_);
            status_ = Status::NotLoggedIn;
            groups_.clear();
            message_.clear();
        }
        if (host_ != nullptr) {
            InvalidateRect(host_, nullptr, FALSE);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Loading;
        message_.clear();
    }
    const std::uint64_t myGen = gen_.load();
    workers_.emplace_back([this, myGen] { worker(myGen); });
}

void RadioPage::reset() {
    // Bump gen so any worker still in flight (e.g. logout fires mid-fetch, or
    // a tab toggle preempted us) discards its result on commit. Don't join
    // here -- workers_ are reaped in the dtor; joining here would block the
    // UI for up to ~46s on rapid toggle.
    gen_.fetch_add(1);
    {
        const std::lock_guard<std::mutex> lk(mu_);
        status_ = Status::Idle;
        groups_.clear();
        message_.clear();
    }
    scrollY_ = 0.0F;
    contentHeight_ = 0.0F;
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void RadioPage::worker(std::uint64_t myGen) {
    HttpClient http;
    ApiClient::Transport transport = [&http](const HttpRequest& r) {
        return http.send(r);
    };

    Status st = Status::Error;
    std::string msg;
    std::vector<RadioGroupInfo> groups;

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
            auto platform = resolveRadioPlatform(platforms);
            if (!platform) {
                msg = "没有支持电台的可用平台";
            } else {
                HttpRequest req;
                req.url = url::buildUrl(base, "/v1/radios",
                                        {{"platform", platform->id}});
                req.connectTimeoutMs = kFetchConnectMs;
                req.readTimeoutMs = kFetchReadMs;
                const HttpResponse resp = client.send(req);
                if (!isHttpOk(resp)) {
                    msg = "无法获取电台列表";
                } else {
                    groups =
                        parseRadioGroups(parseJson(resp.body), platform->id);
                    st = Status::Loaded;
                }
            }
        }
    }

    {
        const std::lock_guard<std::mutex> lk(mu_);
        // ★ R1-S3: gen check inside the lock (TOCTOU defense, same shape as
        // PlaylistDetailPage). If reset() bumped gen between the worker
        // returning from HTTP and grabbing mu_, drop the writes -- status_
        // stays Idle (set by reset), so the next load() can re-fetch.
        if (myGen != gen_.load()) {
            return;
        }
        status_ = st;
        message_ = std::move(msg);
        groups_ = std::move(groups);
    }
    PostMessageW(host_, kDoneMessage, 0, 0);
}

bool RadioPage::onHostMessage(UINT msg, WPARAM /*wp*/, LPARAM /*lp*/) {
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

void RadioPage::onMouseWheel(int wheelDelta, float topInsetDip) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_, topInsetDip));
    scrollY_ = std::clamp(scrollY_ - notches * kScrollStepPx, 0.0F, maxScroll);
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

bool RadioPage::onLeftDown(float xDip, float yDip) {
    if (lastWidthDip_ <= 0.0F) {
        return false;
    }
    const float yContent = yDip + scrollY_;
    RadioInfo target;
    bool hit = false;
    {
        const std::lock_guard<std::mutex> lk(mu_);
        if (status_ != Status::Loaded || !onRadioOpen_) {
            return false;
        }
        const RadioLayout layout = computeRadioLayout(
            groupSizes(groups_), lastWidthDip_, lastMetrics_);
        const std::size_t n = std::min(layout.groups.size(), groups_.size());
        for (std::size_t gi = 0; gi < n && !hit; ++gi) {
            const SectionLayout& sl = layout.groups.at(gi);
            if (!sl.present) {
                continue;
            }
            const std::size_t ic =
                std::min(sl.items.size(), groups_.at(gi).radios.size());
            for (std::size_t i = 0; i < ic; ++i) {
                const LayoutRect& r = sl.items.at(i);
                if (xDip >= r.left && xDip < r.right && yContent >= r.top &&
                    yContent < r.bottom) {
                    target = groups_.at(gi).radios.at(i);
                    hit = true;
                    break;
                }
            }
        }
    }
    if (hit) {
        onRadioOpen_(target);
        return true;
    }
    return false;
}

void RadioPage::paint(ID2D1RenderTarget* rt, const Theme& theme,
                      D2D1_SIZE_F size) {
    const std::lock_guard<std::mutex> lk(mu_);
    lastWidthDip_ = size.width;

    switch (status_) {
        case Status::Idle:
        case Status::Loading:
            drawCentered(rt, theme, size, L"正在加载电台…");
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

    if (groups_.empty()) {
        drawCentered(rt, theme, size, L"暂无电台");
        return;
    }

    const LayoutMetrics m = currentMetrics(theme);
    lastMetrics_ = m;
    const std::vector<std::size_t> sizes = groupSizes(groups_);
    const RadioLayout layout = computeRadioLayout(sizes, size.width, m);
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

    const std::size_t n = std::min(layout.groups.size(), groups_.size());
    for (std::size_t gi = 0; gi < n; ++gi) {
        drawCardSection(
            rn, layout.groups.at(gi), groups_.at(gi).radios,
            utf8ToWide(groups_.at(gi).name), /*square=*/true,
            [](const RadioInfo& r) { return r.name; },
            [](const RadioInfo& /*r*/) { return std::string{}; },
            [](const RadioInfo& r) { return r.cover; });
    }
}

}  // namespace hemusic::ui
