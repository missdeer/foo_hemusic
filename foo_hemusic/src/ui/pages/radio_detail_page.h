#pragma once

// Radio detail page (HEMUSIC-36). Mirrors PlaylistDetailPage in form but with
// only one HTTP call (/v1/radio/songs) and no enqueue stub (HEMUSIC-5 deferred,
// same scope as Playlist/Album detail). Banner shows cover + radio name +
// song count; songs list reuses section_render's Renderer.

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

#include "api/radio.h"
#include "api/song.h"
#include "ui/nav.h"
#include "ui/pages/discover_layout.h"
#include "ui/theme.h"

namespace hemusic::ui {

class RadioDetailPage {
   public:
    // +14 / +15 (after radio_page's +12/+13). MainPanel forwards via
    // onHostMessage.
    static constexpr UINT kDoneMessage = WM_APP + 14;
    static constexpr UINT kCoverReadyMessage = WM_APP + 15;

    RadioDetailPage() = default;
    ~RadioDetailPage();

    RadioDetailPage(const RadioDetailPage&) = delete;
    RadioDetailPage(RadioDetailPage&&) = delete;
    RadioDetailPage& operator=(const RadioDetailPage&) = delete;
    RadioDetailPage& operator=(RadioDetailPage&&) = delete;

    void attach(HWND host) { host_ = host; }

    // Bumps the generation counter (so any in-flight worker drops its result)
    // and starts a new worker. Reads entry.params["id"/"platform"/"title"].
    void enter(const nav::PageEntry& entry);

    // Drops worker writes + clears state (used by MainPanel on tab change /
    // auth change / pop).
    void reset();

    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);
    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);
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
        std::string cover;
    };

    void worker(WorkerArgs args, std::uint64_t myGen);

    HWND host_ = nullptr;

    // Same worker-accumulation pattern as PlaylistDetailPage: join in dtor so
    // navigating between radios doesn't block the UI thread.
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> gen_{0};

    mutable std::mutex mu_;
    Status status_ = Status::Idle;
    // banner content carried from the PageEntry (no /v1/radio meta endpoint --
    // radio name + cover come from the originating RadioInfo).
    std::string title_;
    std::string cover_;
    std::vector<SongInfo> songs_;
    std::string message_;

    // UI thread only:
    float scrollY_ = 0.0F;
    float contentHeight_ = 0.0F;
};

}  // namespace hemusic::ui
