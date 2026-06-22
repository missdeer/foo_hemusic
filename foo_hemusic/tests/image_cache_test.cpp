// Unit tests for the ImageCache hardening added in HEMUSIC-31: per-subscriber
// listener dedup, the Failed-entry negative-cache TTL (with an injected clock),
// and unsubscribe. The Loaded-bitmap path needs a real GPU render target, so it
// is exercised by the live discover page, not here; every test below drives the
// Failed path (empty fetch bytes) or a blocking fetch held at Pending.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ui/image_cache.h"

using hemusic::ui::ImageCache;
using namespace std::chrono_literals;

namespace {

constexpr std::chrono::seconds kWaitTimeout{5};
constexpr std::size_t kCapacity = 8;
constexpr std::size_t kWorkers = 1;
constexpr int kRepeatRequests = 5;
constexpr int kPollIters = 500;
constexpr std::chrono::milliseconds kPollStep{2};
constexpr std::chrono::seconds kTtl{10};
constexpr std::chrono::seconds kWithinTtl{5};
constexpr std::chrono::seconds kPastTtl{11};

// Counts listener resolutions and lets a test block until N have fired.
struct Probe {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;

    void notify() {
        {
            const std::lock_guard<std::mutex> lock(m);
            ++count;
        }
        cv.notify_all();
    }
    bool waitFor(int n) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, kWaitTimeout, [&] { return count >= n; });
    }
    int value() {
        const std::lock_guard<std::mutex> lock(m);
        return count;
    }
};

ImageCache::Listener notifier(Probe& p) {
    return [&p] { p.notify(); };
}

// Cache whose fetch always fails (empty bytes) and whose clock is test-driven,
// built here so the TTL test body stays free of the fetch/clock lambdas.
struct FailingCacheFixture {
    std::atomic<int> fetchCalls{0};
    std::atomic<std::int64_t> nowNs{0};
    std::unique_ptr<ImageCache> cache;

    explicit FailingCacheFixture(std::chrono::steady_clock::duration ttl) {
        std::atomic<int>* calls = &fetchCalls;
        std::atomic<std::int64_t>* now = &nowNs;
        ImageCache::FetchFn fetch = [calls](const std::string&) {
            calls->fetch_add(1);
            return std::vector<std::uint8_t>{};  // empty -> Failed
        };
        ImageCache::Clock clock = [now] {
            return std::chrono::steady_clock::time_point(
                std::chrono::nanoseconds(now->load()));
        };
        cache = std::make_unique<ImageCache>(fetch, kCapacity, kWorkers, ttl,
                                             clock);
    }
};

}  // namespace

TEST_CASE("ImageCache dedups listeners per subscriber across repeat requests",
          "[image_cache]") {
    std::atomic<int> fetchCalls{0};
    std::promise<void> gate;
    const std::shared_future<void> gateF = gate.get_future().share();
    ImageCache::FetchFn fetch = [&](const std::string&) {
        fetchCalls.fetch_add(1);
        gateF.wait();  // hold the entry Pending until the test releases
        return std::vector<std::uint8_t>{};  // empty -> Failed
    };
    ImageCache cache(fetch, kCapacity, kWorkers);

    Probe probe;
    const int token = 0;
    const void* subscriber = &token;
    // paint re-asks for the same visible cover every frame: many requests, one
    // subscriber -> the listener must still fire exactly once.
    for (int i = 0; i < kRepeatRequests; ++i) {
        cache.request("u", subscriber, notifier(probe));
    }
    gate.set_value();

    REQUIRE(probe.waitFor(1));
    REQUIRE(probe.value() == 1);      // dedup: one resolution, not many
    REQUIRE(fetchCalls.load() == 1);  // URL fetched once, not per request
}

TEST_CASE("ImageCache stops notifying an unsubscribed subscriber",
          "[image_cache]") {
    std::promise<void> gate;
    const std::shared_future<void> gateF = gate.get_future().share();
    ImageCache::FetchFn fetch = [&](const std::string&) {
        gateF.wait();
        return std::vector<std::uint8_t>{};
    };
    ImageCache cache(fetch, kCapacity, kWorkers);

    Probe probe;
    const int token = 0;
    const void* subscriber = &token;
    cache.request("u", subscriber, notifier(probe));
    cache.unsubscribe(subscriber);
    // unsubscribe happens-before the fetch returns (fetch is blocked on gate),
    // so the listener is gone before onFetched runs.
    gate.set_value();

    // Poll for the entry to settle (bounded) instead of paying the
    // wait-timeout.
    for (int i = 0; i < kPollIters && cache.pendingCount() != 0; ++i) {
        std::this_thread::sleep_for(kPollStep);
    }
    REQUIRE(cache.pendingCount() == 0);
    REQUIRE(probe.value() == 0);
}

TEST_CASE("ImageCache re-fetches a Failed entry only after the TTL",
          "[image_cache]") {
    FailingCacheFixture fx(kTtl);
    Probe probe;
    const int token = 0;
    const void* subscriber = &token;

    // 1) First request -> one fetch -> Failed (timestamped at now = 0).
    fx.cache->request("u", subscriber, notifier(probe));
    REQUIRE(probe.waitFor(1));
    REQUIRE(fx.fetchCalls.load() == 1);

    // 2) Within the TTL -> served from the negative cache, no refetch.
    fx.nowNs.store(std::chrono::nanoseconds(kWithinTtl).count());
    REQUIRE(fx.cache->request("u", subscriber, notifier(probe)) == nullptr);
    REQUIRE(fx.cache->pendingCount() == 0);  // not re-queued
    REQUIRE(fx.fetchCalls.load() == 1);

    // 3) Past the TTL -> the stale Failed entry is retried.
    fx.nowNs.store(std::chrono::nanoseconds(kPastTtl).count());
    fx.cache->request("u", subscriber, notifier(probe));
    REQUIRE(probe.waitFor(2));
    REQUIRE(fx.fetchCalls.load() == 2);
}
