#pragma once

// Radio browse page (HEMUSIC-36, Phase 5 #7a). Hosted windowless inside
// MainPanel's HWND + render target, mirroring DiscoverPage. Loads /v1/platforms
// -> resolves the first radio-capable platform -> /v1/radios -> renders one
// section per RadioGroupInfo with a wrapped square-card grid. Clicking a radio
// card opens RadioDetailPage. Cover art loads async via the shared
// cover ImageCache.

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

#include "api/radio.h"
#include "ui/pages/discover_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

class RadioPage {
   public:
    // WM_APP+1..+11 are taken by existing pages (discover/search/playlist/
    // album/artist detail). Radio takes +12 / +13.
    static constexpr UINT kDoneMessage = WM_APP + 12;
    static constexpr UINT kCoverReadyMessage = WM_APP + 13;

    RadioPage() = default;
    ~RadioPage();

    RadioPage(const RadioPage&) = delete;
    RadioPage(RadioPage&&) = delete;
    RadioPage& operator=(const RadioPage&) = delete;
    RadioPage& operator=(RadioPage&&) = delete;

    void attach(HWND host) { host_ = host; }

    // Kicks off the fetch. No-op when a load is already in flight. Sets the
    // not-logged-in state synchronously when unauthenticated.
    void load();

    // Bumps internal state to Idle + drops worker writes (used by MainPanel on
    // auth/tab changes so a stale fetch can't paint stale data). UI thread.
    void reset();

    // Consumes kDoneMessage (worker finished -> repaint from top) and
    // kCoverReadyMessage (cover loaded -> repaint in place); returns true for
    // either. Other messages are ignored.
    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);

    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

    // Mouse left-click in page-content DIP coordinates. Returns true if a
    // radio card was activated (caller doesn't need SetCapture; no draggable
    // controls).
    bool onLeftDown(float xDip, float yDip);

    void onMouseWheel(int wheelDelta, float topInsetDip);

    // Wire-once callback fired when the user clicks a radio card. MainPanel
    // pushes a RadioDetail PageEntry from this.
    void setOnRadioOpen(std::function<void(const RadioInfo&)> cb) {
        onRadioOpen_ = std::move(cb);
    }

   private:
    enum class Status : std::uint8_t {
        Idle,
        Loading,
        NotLoggedIn,
        Error,
        Loaded,
    };

    void worker(std::uint64_t myGen);

    HWND host_ = nullptr;
    // Workers accumulate here so rapid tab toggles don't block the UI thread
    // on a still-running fetch's join. gen_ is bumped on reset(); the worker
    // re-checks gen under mu_ before committing and drops on mismatch. Aligns
    // with PlaylistDetailPage's pattern. (Earlier shape with a single thread_
    // + loading_ atomic deadlocked perpetual-Loading on rapid Radio↔other tab
    // toggles — see review R2.)
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> gen_{0};

    mutable std::mutex mu_;  // guards worker-written state
    Status status_ = Status::Idle;
    std::vector<RadioGroupInfo> groups_;
    std::string message_;

    // UI-thread only:
    float scrollY_ = 0.0F;
    float contentHeight_ = 0.0F;
    float lastWidthDip_ = 0.0F;
    LayoutMetrics lastMetrics_;
    std::function<void(const RadioInfo&)> onRadioOpen_;
};

}  // namespace hemusic::ui
