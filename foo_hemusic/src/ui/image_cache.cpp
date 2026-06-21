#include "ui/image_cache.h"

#include <objbase.h>

#include <exception>
#include <utility>
#include <vector>

#include "ui/d2d.h"

namespace hemusic::ui {

using Microsoft::WRL::ComPtr;

namespace {

// RAII pairing of CoInitializeEx with CoUninitialize. Skips uninit when the
// init returned RPC_E_CHANGED_MODE (apartment already chosen by someone
// else, our call did nothing) or another failure -- CoUninitialize must
// only match a successful init on the same thread.
class ComMtaScope {
   public:
    ComMtaScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ComMtaScope(const ComMtaScope&) = delete;
    ComMtaScope(ComMtaScope&&) = delete;
    ComMtaScope& operator=(const ComMtaScope&) = delete;
    ComMtaScope& operator=(ComMtaScope&&) = delete;
    ~ComMtaScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    bool ok() const { return SUCCEEDED(hr_); }

   private:
    HRESULT hr_;
};

}  // namespace

ImageCache::ImageCache(FetchFn fetch, std::size_t capacity,
                       std::size_t workerCount)
    : fetch_(std::move(fetch)), capacity_(capacity) {
    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ImageCache::~ImageCache() {
    {
        std::lock_guard lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

ComPtr<IWICBitmapSource> ImageCache::request(const std::string& url,
                                             const void* subscriber,
                                             Listener listener) {
    std::lock_guard lock(mu_);
    auto it = entries_.find(url);
    if (it != entries_.end()) {
        auto& e = it->second;
        if (e.state == State::Loaded) {
            // Bump MRU: splice the existing list node to the front.
            lru_.splice(lru_.begin(), lru_, e.lruIt);
            return e.bitmap;
        }
        if (e.state == State::Failed) {
            if (e.inLru) {
                lru_.splice(lru_.begin(), lru_, e.lruIt);
            }
            return nullptr;
        }
        // Pending: queue the listener if any.
        if (listener) {
            e.subscribers.push_back({subscriber, std::move(listener)});
        }
        return nullptr;
    }
    // Brand-new URL: park a Pending entry, queue the fetch, register listener.
    Entry fresh;
    fresh.state = State::Pending;
    if (listener) {
        fresh.subscribers.push_back({subscriber, std::move(listener)});
    }
    entries_.emplace(url, std::move(fresh));
    queue_.push_back(url);
    cv_.notify_one();
    return nullptr;
}

ComPtr<IWICBitmapSource> ImageCache::get(const std::string& url) const {
    std::lock_guard lock(mu_);
    auto it = entries_.find(url);
    if (it == entries_.end()) {
        return nullptr;
    }
    if (it->second.state != State::Loaded) {
        return nullptr;
    }
    return it->second.bitmap;
}

void ImageCache::unsubscribe(const void* subscriber) {
    std::lock_guard lock(mu_);
    for (auto& [url, e] : entries_) {
        if (e.state != State::Pending) {
            continue;
        }
        std::erase_if(e.subscribers, [subscriber](const Subscriber& s) {
            return s.key == subscriber;
        });
    }
}

void ImageCache::clear() {
    std::lock_guard lock(mu_);
    entries_.clear();
    lru_.clear();
    // Drain queued URLs the workers haven't picked up yet so they don't burn
    // network + decode cycles on results onFetched will discard. URLs that
    // have already been popped (worker mid-fetch) still complete; if a later
    // request() re-creates the same URL's Pending entry before that older
    // completion lands, onFetched will fill it with the older bytes. Image
    // URLs are content-deterministic for us (the URL embeds the token /
    // version), so identical URLs produce identical bytes -- the race is
    // semantically benign. If a future caller introduces a URL whose body
    // can change between requests, that caller needs to invalidate the
    // entry by key, not rely on clear().
    queue_.clear();
}

std::size_t ImageCache::cachedCount() const {
    std::lock_guard lock(mu_);
    return lru_.size();
}

std::size_t ImageCache::pendingCount() const {
    std::lock_guard lock(mu_);
    std::size_t n = 0;
    for (const auto& [url, e] : entries_) {
        if (e.state == State::Pending) {
            ++n;
        }
    }
    return n;
}

void ImageCache::workerLoop() {
    // Each worker owns its MTA apartment so the WIC factory (free-threaded
    // / "Both") is callable from here without marshalling. ComMtaScope
    // pairs init/uninit even when an exception escapes fetch_/decode.
    ComMtaScope com;
    while (true) {
        std::string url;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                break;
            }
            url = std::move(queue_.front());
            queue_.pop_front();
        }
        std::vector<std::uint8_t> bytes;
        try {
            if (fetch_) {
                bytes = fetch_(url);
            }
        } catch (...) {
            // A throwing fetcher must not crash the worker -- treat as
            // empty bytes so onFetched marks the entry Failed. The catch
            // is scoped to fetch_ only: wrapping onFetched too would mean a
            // throwing listener triggers a duplicate onFetched(url, {})
            // and corrupts LRU bookkeeping.
            bytes.clear();
        }
        onFetched(url, std::move(bytes));
    }
}

void ImageCache::onFetched(const std::string& url,
                           std::vector<std::uint8_t> bytes) {
    ComPtr<IWICBitmapSource> bitmap;
    if (!bytes.empty()) {
        bitmap = d2d::decodeImage(bytes.data(), bytes.size());
    }

    std::lock_guard lock(mu_);
    auto it = entries_.find(url);
    if (it == entries_.end()) {
        // clear() removed us mid-flight; nothing to notify, no LRU entry.
        return;
    }
    auto& e = it->second;
    // Late arrivals: a second completion for the same URL (clear()+re-
    // request races, or a throwing listener that previously caused a
    // duplicated workerLoop catch path) finds the entry already settled.
    // Push-front-ing again would dangle the prior lruIt and double-count
    // capacity; drop the duplicate result quietly.
    if (e.state != State::Pending) {
        return;
    }
    if (bitmap) {
        e.state = State::Loaded;
        e.bitmap = bitmap;
    } else {
        e.state = State::Failed;
    }
    lru_.push_front(url);
    e.lruIt = lru_.begin();
    e.inLru = true;
    auto pending = std::move(e.subscribers);
    e.subscribers.clear();
    evictIfNeededLocked();
    // Listeners fire under the lock so a concurrent unsubscribe() can't
    // observe "I removed this listener" while we still hold a copy about
    // to fire. Listeners are documented as PostMessage / InvalidateRect
    // shaped -- bounded, no cache re-entry, no synchronous wait on the UI
    // thread. We accept that this widens the lock-hold time over a stricter
    // "snapshot + release + fire" design, because that pattern reopens the
    // very UAF window round 1's review caught.
    for (auto& sub : pending) {
        try {
            sub.listener();
        } catch (...) {
            // One bad listener mustn't starve the rest of the wait list.
        }
    }
}

void ImageCache::evictIfNeededLocked() {
    while (lru_.size() > capacity_) {
        const std::string victim = std::move(lru_.back());
        lru_.pop_back();
        entries_.erase(victim);
    }
}

}  // namespace hemusic::ui
