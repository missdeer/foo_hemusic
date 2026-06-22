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

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "api/discover.h"
#include "api/platforms.h"
#include "core/session.h"
#include "net/api_client.h"
#include "net/http_client.h"
#include "net/url_codec.h"
#include "ui/cover_cache.h"
#include "ui/d2d.h"
#include "ui/image_cache.h"
#include "ui/pages/discover_layout.h"

// No SDK headers here: Session / ApiClient / HttpClient are SDK-free and d2d /
// theme are plain Win32 + Direct2D, so nothing pulls WinSock2 and the lone
// <windows.h> is unambiguous.

namespace hemusic::ui {

namespace {

using Microsoft::WRL::ComPtr;

constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;  // exclusive (2xx)

// Cover/data fetch timeouts (shorter than the 20s/30s default) so closing the
// panel mid-fetch bounds the worker join at teardown (review: AGY/Codex).
constexpr int kFetchConnectMs = 8000;
constexpr int kFetchReadMs = 15000;

// Pixels scrolled per wheel notch (WHEEL_DELTA units).
constexpr float kScrollStepPx = 120.0F;
// Section title band height as a factor of the title font size.
constexpr float kTitleBandFactor = 1.6F;
// Song-row index column width (DIP) and card cover->text gap.
constexpr float kIndexWidth = 28.0F;
constexpr float kCardInnerGap = 4.0F;
constexpr float kSubGap = 2.0F;  // title line -> sub line
constexpr float kVideoAspect = 9.0F / 16.0F;
constexpr float kStrokeWidth = 1.0F;
constexpr float kRowThumbPad = 4.0F;  // song-row cover inset (top+bottom)

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
                                     bool centered, bool ellipsis) {
    ComPtr<IDWriteTextFormat> tf;
    IDWriteFactory* dw = d2d::dwriteFactory();
    if (dw == nullptr) {
        return tf;
    }
    dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         size, L"", tf.GetAddressOf());
    if (!tf) {
        return tf;
    }
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (centered) {
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (ellipsis) {
        // Ellipsize overlong titles so they can't spill across cells (the
        // no-clip scroll path relies on per-draw clipping, not a region clip).
        DWRITE_TRIMMING trim{};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(tf.Get(),
                                                     sign.GetAddressOf()))) {
            tf->SetTrimming(&trim, sign.Get());
        }
    }
    return tf;
}

// Clips text to its rect (D2D1_DRAW_TEXT_OPTIONS_CLIP) so a long title can't
// bleed past its cell into a neighbour.
void drawText(ID2D1RenderTarget* rt, IDWriteTextFormat* tf,
              const std::wstring& text, ID2D1Brush* brush,
              const D2D1_RECT_F& rect) {
    if (tf == nullptr || brush == nullptr || text.empty()) {
        return;
    }
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), tf, rect,
                  brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Draws `bmp` to fill `dst` without distortion: center-crops the source to the
// destination's aspect ratio (cover-fit). Music covers are square but the
// destination may be 16:9 (MV cards), so a plain stretch would skew them.
void drawCoverBitmap(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp,
                     const D2D1_RECT_F& dst) {
    const D2D1_SIZE_F bs = bmp->GetSize();
    const float tw = dst.right - dst.left;
    const float th = dst.bottom - dst.top;
    if (bs.width <= 0.0F || bs.height <= 0.0F || tw <= 0.0F || th <= 0.0F) {
        return;
    }
    const float targetAspect = tw / th;
    const float srcAspect = bs.width / bs.height;
    D2D1_RECT_F srcRect{};
    if (srcAspect > targetAspect) {  // source wider: crop left/right
        const float w = bs.height * targetAspect;
        const float x = (bs.width - w) * 0.5F;
        srcRect = D2D1::RectF(x, 0.0F, x + w, bs.height);
    } else {  // source taller: crop top/bottom
        const float h = bs.width / targetAspect;
        const float y = (bs.height - h) * 0.5F;
        srcRect = D2D1::RectF(0.0F, y, bs.width, y + h);
    }
    rt->DrawBitmap(bmp, dst, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                   &srcRect);
}

// Bundles the per-paint Direct2D resources + scroll/viewport so the section
// loops below stay short. Translates content rects by -scrollY and culls
// off-viewport items.
struct Renderer {
    ID2D1RenderTarget* rt = nullptr;
    ComPtr<IDWriteTextFormat> titleFmt;
    ComPtr<IDWriteTextFormat> rowFmt;
    ComPtr<IDWriteTextFormat> subFmt;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> subBrush;
    ImageCache* covers = nullptr;      // shared cover cache (may be null)
    const void* subscriber = nullptr;  // owner key for cover listeners
    HWND host = nullptr;               // posted to when a cover finishes
    float scrollY = 0;
    float viewportH = 0;
    float rowTitleH = 0;

    D2D1_RECT_F screen(const LayoutRect& r) const {
        return D2D1::RectF(r.left, r.top - scrollY, r.right,
                           r.bottom - scrollY);
    }
    bool visible(const D2D1_RECT_F& s) const {
        return s.bottom >= 0.0F && s.top <= viewportH;
    }
    void sectionTitle(const SectionLayout& s, const std::wstring& label) const {
        const D2D1_RECT_F sr = screen(s.title);
        if (visible(sr)) {
            drawText(rt, titleFmt.Get(), label, textBrush.Get(), sr);
        }
    }
    // Real cover if cached, else queues an async fetch and draws a placeholder
    // box until kCoverReadyMessage triggers a repaint. The makeBitmap upload is
    // per-paint (device-dependent resource, never cached across targets) --
    // fine for the discover page's discrete repaints.
    void drawCover(const D2D1_RECT_F& dst, const std::string& url) const {
        if (covers != nullptr && !url.empty()) {
            const HWND h = host;
            ComPtr<IWICBitmapSource> src =
                covers->request(url, subscriber, [h] {
                    PostMessageW(h, DiscoverPage::kCoverReadyMessage, 0, 0);
                });
            if (src) {
                ComPtr<ID2D1Bitmap> bmp = d2d::makeBitmap(rt, src.Get());
                if (bmp) {
                    drawCoverBitmap(rt, bmp.Get(), dst);
                    return;
                }
            }
        }
        rt->DrawRectangle(dst, subBrush.Get(), kStrokeWidth);
    }
    void songRow(const D2D1_RECT_F& r, int index, const std::wstring& name,
                 const std::wstring& artist,
                 const std::string& coverUrl) const {
        drawText(rt, subFmt.Get(), std::to_wstring(index), subBrush.Get(),
                 D2D1::RectF(r.left, r.top, r.left + kIndexWidth, r.bottom));
        const float thumbX = r.left + kIndexWidth;
        const float side =
            std::max(0.0F, (r.bottom - r.top) - 2.0F * kRowThumbPad);
        drawCover(D2D1::RectF(thumbX, r.top + kRowThumbPad, thumbX + side,
                              r.bottom - kRowThumbPad),
                  coverUrl);
        const float tx = thumbX + side + kCardInnerGap;
        const float titleBottom = r.top + kSubGap + rowTitleH;
        drawText(rt, rowFmt.Get(), name, textBrush.Get(),
                 D2D1::RectF(tx, r.top + kSubGap, r.right, titleBottom));
        drawText(rt, subFmt.Get(), artist, subBrush.Get(),
                 D2D1::RectF(tx, titleBottom, r.right, r.bottom));
    }
    void card(const D2D1_RECT_F& r, bool square, const std::wstring& title,
              const std::wstring& sub, const std::string& coverUrl) const {
        const float w = r.right - r.left;
        const float coverH = square ? w : w * kVideoAspect;
        drawCover(D2D1::RectF(r.left, r.top, r.right, r.top + coverH),
                  coverUrl);
        const float ty = r.top + coverH + kCardInnerGap;
        const float titleBottom = ty + rowTitleH;
        drawText(rt, rowFmt.Get(), title, textBrush.Get(),
                 D2D1::RectF(r.left, ty, r.right, titleBottom));
        drawText(rt, subFmt.Get(), sub, subBrush.Get(),
                 D2D1::RectF(r.left, titleBottom, r.right, r.bottom));
    }
};

void drawSongSection(const Renderer& rn, const SectionLayout& sl,
                     const std::vector<SongInfo>& songs) {
    if (!sl.present) {
        return;
    }
    rn.sectionTitle(sl, L"新歌速递");
    const size_t n = std::min(sl.items.size(), songs.size());
    for (size_t i = 0; i < n; ++i) {
        const D2D1_RECT_F s = rn.screen(sl.items.at(i));
        if (!rn.visible(s)) {
            continue;
        }
        rn.songRow(s, static_cast<int>(i) + 1, utf8ToWide(songs.at(i).name),
                   utf8ToWide(songArtistText(songs.at(i))), songs.at(i).cover);
    }
}

// Draws a wrapped card grid: titleFn / subFn / coverFn map each item to its
// display strings + cover URL. Keeps paint() flat (one call per section).
template <class T, class TitleFn, class SubFn, class CoverFn>
void drawCardSection(const Renderer& rn, const SectionLayout& sl,
                     const std::vector<T>& items, const std::wstring& label,
                     bool square, TitleFn titleFn, SubFn subFn,
                     CoverFn coverFn) {
    if (!sl.present) {
        return;
    }
    rn.sectionTitle(sl, label);
    const size_t n = std::min(sl.items.size(), items.size());
    for (size_t i = 0; i < n; ++i) {
        const D2D1_RECT_F s = rn.screen(sl.items.at(i));
        if (!rn.visible(s)) {
            continue;
        }
        rn.card(s, square, utf8ToWide(titleFn(items.at(i))),
                utf8ToWide(subFn(items.at(i))), coverFn(items.at(i)));
    }
}

// Centered single-line message (loading / not-logged-in / error / empty).
void drawCentered(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size,
                  const std::wstring& text) {
    ComPtr<IDWriteTextFormat> tf =
        makeFormat(theme.fontFamily, theme.rowTitleSize, /*centered=*/true,
                   /*ellipsis=*/false);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(theme.secondaryText,
                                         brush.GetAddressOf()))) {
        return;
    }
    drawText(rt, tf.Get(), text, brush.Get(),
             D2D1::RectF(0.0F, 0.0F, size.width, size.height));
}

// Viewport height in DIPs, matching the units of contentHeight_ (which comes
// from rt->GetSize()). GetClientRect is physical pixels, so divide by the
// window's DPI scale -- otherwise high-DPI panels clamp the scroll too early
// and the bottom content stays unreachable.
float viewportHeightDip(HWND hwnd) {
    RECT rc{};
    if (hwnd == nullptr || GetClientRect(hwnd, &rc) == 0) {
        return 0.0F;
    }
    const float scale = d2d::dpiScaleForWindow(hwnd);
    return static_cast<float>(rc.bottom - rc.top) /
           (scale > 0.0F ? scale : 1.0F);
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

void DiscoverPage::onMouseWheel(int wheelDelta) {
    const float notches = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    const float maxScroll =
        std::max(0.0F, contentHeight_ - viewportHeightDip(host_));
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
    rn.rowTitleH = theme.rowTitleSize + kSubGap;
    rn.covers =
        coverCache();  // null before init / after shutdown -> placeholder
    rn.subscriber = this;
    rn.host = host_;

    drawSongSection(rn, layout.songs, songs_);
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
