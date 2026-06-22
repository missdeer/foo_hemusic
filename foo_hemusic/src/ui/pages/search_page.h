#pragma once

// Comprehensive search page (HEMUSIC-14, slice 1). Hosted windowless inside
// MainPanel's HWND + render target, exactly like DiscoverPage: it owns no
// window and no canvas. The non-scrolling search input box is a Win32 EDIT
// owned by MainPanel (chrome above this content area); MainPanel hands a
// submitted keyword to search(). The page runs the blocking GET /v1/search on a
// worker thread and signals completion via PostMessage to the host (mirroring
// discover_page's worker model). It renders the best-match block + five typed
// sections (song / playlist / album / artist / video) reusing the shared
// section_render Renderer; cover art loads asynchronously through the shared
// cover ImageCache.
//
// The category sub-tabs, pagination, and the suggestion overlay are follow-up
// tickets; this slice only renders the comprehensive page.

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

#include "api/album.h"
#include "api/artist.h"
#include "api/mv.h"
#include "api/playlist.h"
#include "api/search.h"
#include "api/song.h"
#include "ui/theme.h"

namespace hemusic::ui {

class SearchPage {
   public:
    // Posted by the worker to the host HWND when the search resolves; MainPanel
    // forwards it to onHostMessage. WM_APP+1..+3 are taken by DiscoverPage /
    // MainPanel, so search takes +4 (done) and +5 (cover ready).
    static constexpr UINT kDoneMessage = WM_APP + 4;
    static constexpr UINT kCoverReadyMessage = WM_APP + 5;

    SearchPage() = default;
    ~SearchPage();

    SearchPage(const SearchPage&) = delete;
    SearchPage(SearchPage&&) = delete;
    SearchPage& operator=(const SearchPage&) = delete;
    SearchPage& operator=(SearchPage&&) = delete;

    // Binds the host window used for repaint notifications. Call before
    // search().
    void attach(HWND host) { host_ = host; }

    // Runs a comprehensive search for `keyword` (UTF-16 from the EDIT box) on a
    // worker thread. No-op while a search is already in flight or when keyword
    // is blank. Sets the not-logged-in state synchronously when
    // unauthenticated.
    void search(const std::wstring& keyword);

    // Resets to the idle prompt (e.g. after logout / tab switch). No-op while a
    // search is in flight. UI thread only.
    void reset();

    // Consumes kDoneMessage (search finished -> repaint from the top) and
    // kCoverReadyMessage (a cover loaded -> repaint in place), returning true
    // for either. Other messages are ignored.
    bool onHostMessage(UINT msg, WPARAM wp, LPARAM lp);

    // Draws the current state into the host's render target.
    void paint(ID2D1RenderTarget* rt, const Theme& theme, D2D1_SIZE_F size);

    // Scrolls by a raw WM_MOUSEWHEEL delta. `topInsetDip` is the non-scrolling
    // chrome height (tab bar + search box) the host reserves above this
    // content. UI thread only.
    void onMouseWheel(int wheelDelta, float topInsetDip);

   private:
    enum class Status : std::uint8_t {
        Idle,
        Loading,
        NotLoggedIn,
        Error,
        Loaded,
    };

    void worker(std::string keyword);

    HWND host_ = nullptr;
    std::thread worker_;
    std::atomic<bool> loading_{false};

    // mu_ guards the worker-written state (status_/message_/keyword_ + the
    // result vectors). scrollY_ / contentHeight_ are UI-thread only.
    mutable std::mutex mu_;
    Status status_ = Status::Idle;
    std::string keyword_;  // UTF-8; echoed in the "<keyword> 搜索结果" title
    std::vector<BestMatchItem> bestMatch_;
    std::vector<SongInfo> songs_;
    std::vector<PlaylistInfo> playlists_;
    std::vector<AlbumInfo> albums_;
    std::vector<ArtistInfo> artists_;
    std::vector<MvInfo> videos_;
    std::string message_;  // error / status detail

    float scrollY_ = 0.0F;        // vertical scroll offset (UI thread)
    float contentHeight_ = 0.0F;  // last measured content height (UI thread)
};

}  // namespace hemusic::ui
