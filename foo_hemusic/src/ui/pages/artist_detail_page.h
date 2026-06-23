#pragma once

// Artist detail page (HEMUSIC-17). Hosted windowless inside MainPanel's HWND
// + render target, mirroring PlaylistDetailPage / AlbumDetailPage. Three
// independent worker threads paginate the per-tab endpoints:
//   GET /v1/artist            -> banner meta (cover/alias/counts/description)
//   GET /v1/artist/songs      -> Songs tab list
//   GET /v1/artist/albums     -> Albums tab grid
//   GET /v1/artist/mvs        -> MVs tab grid
// Each list/grid worker fetches up to kMaxPages pages while has_more is true
// and commits incrementally so the user sees the first page quickly. Album
// cards are clickable (push AlbumDetail); MV cards are silent for now
// (MV detail is a Phase 2 follow-up).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/album.h"
#include "api/artist.h"
#include "api/mv.h"
#include "api/song.h"
#include "ui/nav.h"
#include "ui/pages/detail_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

struct Renderer;  // section_render.h; full def only needed in the .cpp

class ArtistDetailPage {
   public:
    // WM_APP+1..+9 are taken by Discover/Search/MainPanel/PlaylistDetail/
    // AlbumDetail. ArtistDetail takes +10 (done) and +11 (cover ready).
    static constexpr UINT kDoneMessage = WM_APP + 10;
    static constexpr UINT kCoverReadyMessage = WM_APP + 11;

    ArtistDetailPage() = default;
    ~ArtistDetailPage();

    ArtistDetailPage(const ArtistDetailPage&) = delete;
    ArtistDetailPage(ArtistDetailPage&&) = delete;
    ArtistDetailPage& operator=(const ArtistDetailPage&) = delete;
    ArtistDetailPage& operator=(ArtistDetailPage&&) = delete;

    void attach(HWND host) { host_ = host; }

    // Switches the page to an artist identified by entry.params["id" /
    // "platform" / "title"]. Bumps the generation counter (so in-flight
    // workers discard their results), resets all tab state, and starts the
    // banner + Songs workers (Songs is the default tab).
    void enter(const nav::PageEntry& entry);

    // Bumps the generation counter and clears the worker-written state.
    // Triggered by MainPanel on auth changes / pop-back so stale results from
    // logged-in workers can't paint over the navigated-to state.
    void reset();

    // Wire-once: clicking an album card pushes the AlbumDetail page.
    void setOnAlbumOpen(std::function<void(const AlbumInfo&)> cb) {
        onAlbumOpen_ = std::move(cb);
    }

    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);
    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

    bool onLeftDown(float xDip, float yDip);

    void onMouseWheel(int wheelDelta, float topInsetDip);

   private:
    enum class Status : std::uint8_t {
        Idle,
        Loading,
        NotLoggedIn,
        Error,
        Loaded,
    };

    // Per-list-tab state. The Status here is independent of the banner status
    // -- banner shows even while the songs list is still loading.
    struct ListStatus {
        Status status = Status::Idle;
        std::string message;   // error detail
        bool started = false;  // worker spawned at least once
    };

    struct WorkerArgs {
        std::string id;
        std::string platform;
        std::string title;
    };

    // Builds the page's Session-backed transport, runs `task`, and at commit
    // time checks the gen counter to discard superseded results. `task` is
    // invoked on the worker thread with an ApiClient + base URL.
    void bannerWorker(WorkerArgs args, std::uint64_t myGen);
    void songsWorker(WorkerArgs args, std::uint64_t myGen);
    void albumsWorker(WorkerArgs args, std::uint64_t myGen);
    void mvsWorker(WorkerArgs args, std::uint64_t myGen);

    // Kicks off the worker for `tab` if not started yet. Caller must already
    // hold mu_ -> no-op when started_, otherwise sets started_ + kicks the
    // thread.
    void ensureTabLoaded(ArtistTab tab);

    bool hitTab(float xDip, float yDipContent, std::size_t& outIdx) const;

    // Paint helpers (factored to keep paint() under the cognitive-complexity
    // budget). All assume mu_ is held by the caller (paint()).
    bool drawBannerStatusOverlay(ID2D1RenderTarget* rt, const Theme& theme,
                                 D2D1_SIZE_F size) const;
    std::size_t activeItemCount() const;
    const ListStatus& activeListState() const;
    // Draws "正在加载…" / "加载失败" / "暂无…" in the body slot when the
    // active tab has no items. Returns true if it drew (caller skips body).
    bool drawBodyStatusIfEmpty(ID2D1RenderTarget* rt, const Renderer& rn,
                               const ArtistTabBarLayout& bar,
                               const LayoutMetrics& m,
                               std::size_t itemCount) const;
    void drawActiveBody(const Renderer& rn, const SectionLayout& body) const;

    HWND host_ = nullptr;

    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> gen_{0};

    mutable std::mutex mu_;
    // Banner state (shared across tabs).
    Status bannerStatus_ = Status::Idle;
    std::string bannerMessage_;
    ArtistInfo info_;
    // Per-tab lists + status.
    ListStatus songsState_;
    std::vector<SongInfo> songs_;
    ListStatus albumsState_;
    std::vector<AlbumInfo> albums_;
    ListStatus mvsState_;
    std::vector<MvInfo> mvs_;
    // Args carried so a tab-switch can lazily spawn the right worker.
    WorkerArgs args_;

    // UI-thread only (no mu_):
    ArtistTab activeTab_ = ArtistTab::Songs;
    float scrollY_ = 0.0F;
    float contentHeight_ = 0.0F;
    float lastWidthDip_ = 0.0F;
    LayoutMetrics lastMetrics_;

    std::function<void(const AlbumInfo&)> onAlbumOpen_;
};

}  // namespace hemusic::ui
