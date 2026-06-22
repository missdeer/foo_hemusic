#pragma once

// Discover page (PLAN.md Phase 4 final step: minimal discover_page). Loads
// /v1/platforms -> resolves the discover-capable platform -> /v1/page/discover
// and renders the first section (new songs) as a text list. Richer cards /
// covers / the other three sections + scrolling come in Phase 5.
//
// Hosted inside MainPanel's HWND + render target: it owns no window and no
// canvas. MainPanel forwards paint / the done-message; DiscoverPage runs the
// blocking fetch on a worker thread and signals completion via PostMessage to
// the host (mirroring login_dlg's worker model). The dtor joins the worker
// before any member is torn down, so worker writes stay valid throughout.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/song.h"
#include "ui/theme.h"

namespace hemusic::ui {

class DiscoverPage {
   public:
    // Posted by the worker to the host HWND when the fetch resolves; MainPanel
    // forwards it to onHostMessage so the page repaints with fresh state.
    static constexpr UINT kDoneMessage = WM_APP + 1;

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

    // Handles a host message. Returns true when it consumed kDoneMessage (the
    // worker finished): repaints the host. Other messages are ignored.
    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);

    // Draws the current state into the host's render target. Called from
    // MainPanel's WM_PAINT inside its canvas paint callback.
    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

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

    mutable std::mutex mu_;
    Status status_ = Status::Idle;
    std::vector<SongInfo> songs_;  // new-songs section
    std::string message_;          // error / status detail
};

}  // namespace hemusic::ui
