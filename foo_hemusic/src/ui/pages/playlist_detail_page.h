#pragma once

// Playlist detail page (HEMUSIC-15). Hosted windowless inside MainPanel's HWND
// + render target, mirroring DiscoverPage / SearchPage. Two sequential GETs
// (/v1/playlist meta + /v1/playlist/songs full list) on a worker thread; the
// worker writes back under mu_ guarded by a generation counter so navigating to
// a new playlist while an older fetch is in flight discards the stale result
// instead of clobbering the live state.
//
// Renders a banner (cover left, title / creator / counts / "全部入列" button
// right) plus a scrolling song list reusing section_render's Renderer. The
// enqueue button is a stub (HEMUSIC-5 / playlist_writer is deferred) -- the
// page surfaces it via setOnEnqueueAll(); MainPanel currently wires that to a
// popup_message::g_show notice.

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

#include "api/playlist.h"
#include "api/song.h"
#include "ui/nav.h"
#include "ui/pages/discover_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

class PlaylistDetailPage {
   public:
    // Posted by a worker to the host HWND when its fetch resolves; MainPanel
    // forwards via onHostMessage. WM_APP+1..+5 are taken by Discover/Search/
    // MainPanel, so this page takes +6 (done) and +7 (cover ready).
    static constexpr UINT kDoneMessage = WM_APP + 6;
    static constexpr UINT kCoverReadyMessage = WM_APP + 7;

    PlaylistDetailPage() = default;
    ~PlaylistDetailPage();

    PlaylistDetailPage(const PlaylistDetailPage&) = delete;
    PlaylistDetailPage(PlaylistDetailPage&&) = delete;
    PlaylistDetailPage& operator=(const PlaylistDetailPage&) = delete;
    PlaylistDetailPage& operator=(PlaylistDetailPage&&) = delete;

    // Binds the host window used for repaint notifications. Call before
    // enter().
    void attach(HWND host) { host_ = host; }

    // Switches the page to a playlist identified by entry.params["id" /
    // "platform" / "title"]. Bumps the generation counter (so any in-flight
    // worker that hasn't checked in yet discards its result) and starts a new
    // worker. Sets the not-logged-in state synchronously when unauthenticated.
    void enter(const nav::PageEntry& entry);

    // Bumps the generation counter and clears the worker-written state.
    // Triggered by MainPanel on auth changes so stale results from logged-in
    // workers can't paint after logout. UI thread only.
    void reset();

    // Wire-once callback fired when the user clicks the enqueue button. Stub
    // payload (PlaylistInfo + songs) is the same one HEMUSIC-5 will hand to
    // playlist_writer; passing it now avoids reshaping the signature later.
    void setOnEnqueueAll(
        std::function<void(const PlaylistInfo&, const std::vector<SongInfo>&)>
            cb) {
        onEnqueueAll_ = std::move(cb);
    }

    // Consumes kDoneMessage (worker finished -> repaint from top) and
    // kCoverReadyMessage (a cover loaded -> repaint in place), returning true
    // for either. Other messages are ignored.
    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);

    // Draws the current state into the host's render target.
    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

    // Mouse events arrive in page-content DIP coordinates (MainPanel already
    // subtracted contentTopDip()). onLeftDown returns true when the page wants
    // the host to SetCapture (i.e. the click started on the enqueue button).
    bool onLeftDown(float xDip, float yDip);
    bool onMouseMove(float xDip, float yDip);
    bool onLeftUp(float xDip, float yDip);
    void onMouseLeave();
    void onCaptureLost();  // WM_CAPTURECHANGED

    // Scrolls by a raw WM_MOUSEWHEEL delta. `topInsetDip` is the non-scrolling
    // chrome height (tab bar) reserved by the host above this content. UI
    // thread only.
    void onMouseWheel(int wheelDelta, float topInsetDip);

   private:
    enum class Status : std::uint8_t {
        Idle,
        Loading,
        NotLoggedIn,
        Error,
        Loaded,
    };

    // Carries the parameters parsed from a PageEntry into the worker so it can
    // build URL queries + retain the title fallback.
    struct WorkerArgs {
        std::string id;
        std::string platform;
        std::string title;
    };

    void worker(WorkerArgs args, std::uint64_t myGen);
    void joinFinishedWorkers();  // dtor + enter() sweep (best-effort)
    bool hitEnqueueButton(float xDip, float yDipContent) const;

    HWND host_ = nullptr;

    // workers_ holds every worker thread we've spawned so the destructor can
    // join them all (gen-superseded threads still need to be reaped before the
    // DLL unloads). gen_ is bumped on enter()/reset(); the worker's late check
    // (inside mu_) discards results from any non-current generation.
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> gen_{0};

    mutable std::mutex mu_;  // guards everything in this block:
    Status status_ = Status::Idle;
    PlaylistInfo info_;
    std::vector<SongInfo> songs_;
    std::string message_;   // error / status detail
    std::string activeId_;  // current playlist id (UI title fallback)
    std::string activePlatform_;

    // UI-thread only (no mu_ needed):
    float scrollY_ = 0.0F;
    float contentHeight_ = 0.0F;
    float lastWidthDip_ = 0.0F;
    LayoutMetrics lastMetrics_;  // mirrors paint's metrics for hit-test (★ M4)
    bool btnHover_ = false;
    bool btnPressing_ = false;  // mouse went down on the button, still held

    std::function<void(const PlaylistInfo&, const std::vector<SongInfo>&)>
        onEnqueueAll_;
};

}  // namespace hemusic::ui
