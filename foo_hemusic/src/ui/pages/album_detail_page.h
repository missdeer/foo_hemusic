#pragma once

// Album detail page (HEMUSIC-16). Hosted windowless inside MainPanel's HWND
// + render target, mirroring PlaylistDetailPage. A single GET /v1/album
// returns both meta and the song list, so the worker issues one request
// (vs PlaylistDetailPage's two) and the parser produces an AlbumInfo with
// songs already populated.
//
// Renders a banner (cover left, title / artists+publish_time / counts / "全部
// 入列" button right) plus a scrolling song list reusing section_render's
// Renderer + DetailLayout. The enqueue button is a stub (HEMUSIC-5 /
// playlist_writer is deferred) -- the page surfaces it via setOnEnqueueAll();
// MainPanel wires that to a popup_message::g_show notice for now.

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
#include "api/song.h"
#include "ui/nav.h"
#include "ui/pages/discover_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

class AlbumDetailPage {
   public:
    // Posted by a worker to the host HWND when its fetch resolves; MainPanel
    // forwards via onHostMessage. WM_APP+1..+7 are taken by Discover/Search/
    // MainPanel/PlaylistDetail, so this page takes +8 (done) and +9 (cover
    // ready).
    static constexpr UINT kDoneMessage = WM_APP + 8;
    static constexpr UINT kCoverReadyMessage = WM_APP + 9;

    AlbumDetailPage() = default;
    ~AlbumDetailPage();

    AlbumDetailPage(const AlbumDetailPage&) = delete;
    AlbumDetailPage(AlbumDetailPage&&) = delete;
    AlbumDetailPage& operator=(const AlbumDetailPage&) = delete;
    AlbumDetailPage& operator=(AlbumDetailPage&&) = delete;

    void attach(HWND host) { host_ = host; }

    // Switches the page to an album identified by entry.params["id" /
    // "platform" / "title"]. Bumps the generation counter (so any in-flight
    // worker that hasn't checked in yet discards its result) and starts a new
    // worker. Sets the not-logged-in state synchronously when unauthenticated.
    void enter(const nav::PageEntry& entry);

    // Bumps the generation counter and clears the worker-written state.
    // Triggered by MainPanel on auth changes so stale results from logged-in
    // workers can't paint after logout. UI thread only.
    void reset();

    // Wire-once callback fired when the user clicks the enqueue button. Stub
    // payload (AlbumInfo) is the same one HEMUSIC-5 will hand to
    // playlist_writer; passing it now avoids reshaping the signature later.
    void setOnEnqueueAll(std::function<void(const AlbumInfo&)> cb) {
        onEnqueueAll_ = std::move(cb);
    }

    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);

    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

    bool onLeftDown(float xDip, float yDip);
    bool onMouseMove(float xDip, float yDip);
    bool onLeftUp(float xDip, float yDip);
    void onMouseLeave();
    void onCaptureLost();

    void onMouseWheel(int wheelDelta, float topInsetDip);

   private:
    enum class Status : std::uint8_t {
        Idle,
        Loading,
        NotLoggedIn,
        Error,
        Loaded,
    };

    struct WorkerArgs {
        std::string id;
        std::string platform;
        std::string title;
    };

    void worker(WorkerArgs args, std::uint64_t myGen);
    bool hitEnqueueButton(float xDip, float yDipContent) const;

    HWND host_ = nullptr;

    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> gen_{0};

    mutable std::mutex mu_;
    Status status_ = Status::Idle;
    AlbumInfo info_;
    std::string message_;

    float scrollY_ = 0.0F;
    float contentHeight_ = 0.0F;
    float lastWidthDip_ = 0.0F;
    LayoutMetrics lastMetrics_;
    bool btnHover_ = false;
    bool btnPressing_ = false;

    std::function<void(const AlbumInfo&)> onEnqueueAll_;
};

}  // namespace hemusic::ui
