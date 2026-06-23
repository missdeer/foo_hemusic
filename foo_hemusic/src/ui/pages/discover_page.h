#pragma once

// Discover page (HEMUSIC-13). Loads /v1/platforms -> resolves the
// discover-capable platform -> /v1/page/discover and renders all four sections:
// the new-songs list plus new-album / featured-playlist / featured-MV card
// grids, with vertical mouse-wheel scrolling. Cover art loads asynchronously
// through the shared cover ImageCache (HEMUSIC-31); cells show a placeholder
// box until their bitmap arrives.
//
// Hosted inside MainPanel's HWND + render target: it owns no window and no
// canvas. MainPanel forwards paint / the done-message; DiscoverPage runs the
// blocking fetch on a worker thread and signals completion via PostMessage to
// the host (mirroring login_dlg's worker model). The dtor joins the worker
// (then unsubscribes from the cover cache) before any member is torn down, so
// worker writes stay valid throughout.

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
#include "api/mv.h"
#include "api/playlist.h"
#include "api/song.h"
#include "ui/pages/discover_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

class DiscoverPage {
   public:
    // Posted by the worker to the host HWND when the fetch resolves; MainPanel
    // forwards it to onHostMessage so the page repaints with fresh state.
    static constexpr UINT kDoneMessage = WM_APP + 1;

    // Posted by a cover-cache worker when a requested cover finishes loading.
    // Distinct from kDoneMessage because this must repaint WITHOUT resetting
    // the scroll position (kDoneMessage means "fresh page, jump to top").
    // WM_APP+1/ +2 are taken (kDoneMessage / MainPanel's auth-changed), so this
    // is +3.
    static constexpr UINT kCoverReadyMessage = WM_APP + 3;

    DiscoverPage() = default;
    ~DiscoverPage();

    DiscoverPage(const DiscoverPage&) = delete;
    DiscoverPage(DiscoverPage&&) = delete;
    DiscoverPage& operator=(const DiscoverPage&) = delete;
    DiscoverPage& operator=(DiscoverPage&&) = delete;

    // Binds the host window used for repaint notifications. Call before load().
    void attach(HWND host) { host_ = host; }

    // Kicks off the fetch on a worker thread (no-op if already loading). When
    // not authenticated it sets the not-logged-in state synchronously and skips
    // the worker.
    void load();

    // Handles a host message. Consumes kDoneMessage (worker finished -> repaint
    // from the top) and kCoverReadyMessage (a cover loaded -> repaint in
    // place), returning true for either. Other messages are ignored.
    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);

    // Draws the current state into the host's render target. Called from
    // MainPanel's WM_PAINT inside its canvas paint callback.
    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

    // Scrolls the content by a raw WM_MOUSEWHEEL delta (already the signed
    // short from GET_WHEEL_DELTA_WPARAM). `topInsetDip` is the non-scrolling
    // chrome height (the host's tab bar) reserved above this content, so the
    // scroll clamp matches the reduced viewport paint() draws into. UI thread
    // only.
    void onMouseWheel(int wheelDelta, float topInsetDip);

    // Wire-once callback fired when the user clicks one of the featured
    // playlist cards. MainPanel pushes a PlaylistDetail PageEntry from this.
    void setOnPlaylistOpen(std::function<void(const PlaylistInfo&)> cb) {
        onPlaylistOpen_ = std::move(cb);
    }

    // Wire-once callback fired when the user clicks one of the new-album
    // cards. MainPanel pushes an AlbumDetail PageEntry from this.
    void setOnAlbumOpen(std::function<void(const AlbumInfo&)> cb) {
        onAlbumOpen_ = std::move(cb);
    }

    // Page-content DIP coordinates (MainPanel already subtracted
    // contentTopDip()). Hits the new-album grid first, then the
    // featured-playlist grid; misses are silent. Returns true if a card was
    // activated. UI thread only.
    bool onLeftDown(float xDip, float yDip);

   private:
    enum class Status : std::uint8_t {
        Idle,
        Loading,
        NotLoggedIn,
        Error,
        Loaded,
    };

    void worker();

    HWND host_ = nullptr;
    std::thread worker_;
    std::atomic<bool> loading_{false};

    // mu_ guards the worker-written state (status_/message_ + the four section
    // vectors). scrollY_ / contentHeight_ are touched only on the UI thread
    // (paint / onMouseWheel / the kDoneMessage handler), so they need no lock.
    mutable std::mutex mu_;
    Status status_ = Status::Idle;
    std::vector<SongInfo> songs_;          // new-songs section
    std::vector<AlbumInfo> albums_;        // new-albums grid
    std::vector<PlaylistInfo> playlists_;  // featured-playlists grid
    std::vector<MvInfo> mvs_;              // featured-mvs grid
    std::string message_;                  // error / status detail

    float scrollY_ = 0.0F;        // vertical scroll offset (UI thread)
    float contentHeight_ = 0.0F;  // last measured content height (UI thread)
    float lastWidthDip_ = 0.0F;   // last paint() width, used by hit-test
    LayoutMetrics lastMetrics_;   // last paint() metrics; hit-test must reuse
    std::function<void(const PlaylistInfo&)> onPlaylistOpen_;
    std::function<void(const AlbumInfo&)> onAlbumOpen_;
};

}  // namespace hemusic::ui
