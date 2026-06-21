#pragma once

// URL -> decoded WIC bitmap LRU with an async fetch+decode worker pool.
// Shared by every widget that shows cover / avatar art (PLAN.md Phase 4:
// ui/image_cache.{h,cpp}).
//
// Lifecycle:
//   request(url, this, listener)
//     -> Loaded   : returns the bitmap, marks MRU, listener ignored.
//     -> Failed   : returns null, listener ignored. Re-requesting *will*
//                   refetch only after eviction (Failed entries occupy LRU
//                   slots like Loaded).
//     -> Pending  : returns null, listener queued and fired once when the
//                   fetch completes (success OR failure). Caller redraws and
//                   re-asks via get() / request().
//   unsubscribe(this) on panel teardown -- the listener stops firing for
//   that subscriber. The in-flight fetch isn't cancelled (it's cheap to
//   complete; the result lands in the cache for the next caller).
//
// Threading:
//   request / get / unsubscribe / clear callable from any thread (UI calls
//   them from WM_PAINT; workers call request() on themselves only via
//   listener follow-ups, which we don't currently produce).
//   listener runs on a worker thread, with the cache mutex held. It MUST
//   capture only POD / HWND, never `this` of the subscriber -- the
//   destruction window between unsubscribe() and the actual call cannot be
//   closed otherwise. PostMessage to a destroyed HWND is harmless; an
//   InvalidateRect on a destroyed window is harmless. Listeners MUST NOT
//   call back into the cache (no request / get / unsubscribe / clear from
//   inside) -- the mutex is recursive only to absorb accidental re-entry,
//   not to license a notification fan-out.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hemusic::ui {

class ImageCache {
   public:
    // Fetches the raw image bytes for `url`. Empty result = failure.
    // Invoked on a worker thread, called serially per URL (no rate limit
    // here; supply one in the fetcher if needed).
    using FetchFn =
        std::function<std::vector<std::uint8_t>(const std::string& url)>;

    // "Something changed for the URL you asked about" -- fired exactly once
    // per pending-request resolution per subscriber. Caller redraws.
    using Listener = std::function<void()>;

    static constexpr std::size_t kDefaultCapacity = 128;
    static constexpr std::size_t kDefaultWorkers = 4;

    explicit ImageCache(FetchFn fetch, std::size_t capacity = kDefaultCapacity,
                        std::size_t workerCount = kDefaultWorkers);
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache(ImageCache&&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;
    ImageCache& operator=(ImageCache&&) = delete;

    // Loaded: returns bitmap + bumps MRU. Failed: returns null. Pending /
    // new: returns null + registers listener (+ enqueues fetch if new).
    Microsoft::WRL::ComPtr<IWICBitmapSource> request(const std::string& url,
                                                     const void* subscriber,
                                                     Listener listener);

    // Read-only lookup. No fetch, no listener, no MRU change. Use on
    // re-paint after a listener notification to retrieve the bitmap.
    Microsoft::WRL::ComPtr<IWICBitmapSource> get(const std::string& url) const;

    // Remove every listener owned by this subscriber. In-flight fetches
    // continue but their results just land in the cache without notifying
    // anyone.
    void unsubscribe(const void* subscriber);

    // Drop every cached entry (useful on logout / base URL change). Pending
    // entries stay pending; their listeners still fire when fetches finish.
    void clear();

    // Diagnostics.
    std::size_t cachedCount() const;
    std::size_t pendingCount() const;

   private:
    enum class State : std::uint8_t { Pending, Loaded, Failed };

    struct Subscriber {
        const void* key;
        Listener listener;
    };

    struct Entry {
        State state = State::Pending;
        Microsoft::WRL::ComPtr<IWICBitmapSource> bitmap;
        std::vector<Subscriber> subscribers;
        std::list<std::string>::iterator lruIt;  // valid iff in lru_
        bool inLru = false;
    };

    void workerLoop();
    void onFetched(const std::string& url, std::vector<std::uint8_t> bytes);
    void evictIfNeededLocked();

    FetchFn fetch_;
    std::size_t capacity_;

    // Recursive so the listener invocation in onFetched stays inside the
    // lock without deadlocking on a legitimate (documented-banned, but
    // worth defending against) re-entry from user code.
    mutable std::recursive_mutex mu_;
    std::condition_variable_any cv_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;     // front = MRU. Loaded + Failed entries.
    std::deque<std::string> queue_;  // urls awaiting fetch
    bool stopping_ = false;

    std::vector<std::thread> workers_;
};

}  // namespace hemusic::ui
