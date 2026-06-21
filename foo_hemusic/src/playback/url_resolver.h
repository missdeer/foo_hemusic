#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "net/http_client.h"
#include "playback/hemusic_url.h"

// Resolves a SongRef into a real, time-limited stream URL via GET /v1/song/url,
// caching the result briefly (PLAN 3.3). Direct links expire in minutes, so the
// cache TTL is short (30s default) and the input layer can force a refetch via
// invalidate() when a cached link fails at stream time (the "401/403 重取"
// path). Mirrors HE-Music-Flutter:
//   - request: id/platform/quality(=320)/format(=mp3), User-Agent heAudio
//     (online_api_client.fetchSongUrl + he_audio_handler._fetchSongUrl)
//   - response: { url, format }; empty url -> failure
//     (online_controller.resolveSongUrl)
//   - retry: up to 3 attempts on a transport error, status>=500, or a 2xx with
//     a missing url (he_audio_handler._fetchSongUrlWithRetry /
//     _isRetryableFetchError); a 4xx fails immediately (401 is already handled
//     by the injected transport's refresh, 403 captcha is Phase 6).
//
// Pure logic over an injected transport + clock, so the cache/retry contract is
// unit-testable with fakes and the file stays free of the foobar2000 SDK (lives
// in hemusic_core). In production the transport forwards to ApiClient::send
// (bearer + 401 refresh) and the clock to steady_clock. Not thread-safe: the
// caller serializes access (single-flight lands with Phase 4 concurrency).

namespace hemusic {

// Flutter's audio User-Agent (audio_player_factory.dart heAudioUserAgent); the
// backend gates /v1/song/url and the stream on it (PLAN R2).
inline constexpr std::string_view kHeAudioUserAgent =
    "Mozilla/5.0 (Linux; Android 13; Pixel 6) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";

inline constexpr int kDefaultQuality = 320;
inline constexpr long long kDefaultUrlTtlMs = 30000;
inline constexpr std::size_t kDefaultUrlCacheSize = 128;
inline constexpr int kFetchSongUrlMaxAttempts = 3;

struct SongUrlResolution {
    std::string url;
    std::string format;
};

class UrlResolver {
   public:
    using Transport = std::function<HttpResponse(const HttpRequest&)>;
    using Clock = std::function<long long()>;  // monotonic milliseconds

    UrlResolver(std::string baseUrl, Transport transport, Clock clock,
                long long ttlMs = kDefaultUrlTtlMs,
                std::size_t maxEntries = kDefaultUrlCacheSize);

    // Returns the resolved stream URL for ref (served from cache while fresh),
    // or nullopt on failure (non-retryable HTTP / exhausted retries / missing
    // url). quality/format default to Flutter's 320 / "mp3".
    std::optional<SongUrlResolution> resolve(const SongRef& ref,
                                             int quality = kDefaultQuality,
                                             const std::string& format = "mp3");

    // Drops the cache entry for ref+quality+format so the next resolve
    // refetches (the input layer calls this when playing a cached, now-expired
    // link fails with 401/403 on the stream).
    void invalidate(const SongRef& ref, int quality = kDefaultQuality,
                    const std::string& format = "mp3");

    void clear();
    std::size_t size() const { return lru_.size(); }

   private:
    struct Entry {
        std::string key;
        SongUrlResolution value;
        long long timestampMs = 0;
    };

    std::optional<SongUrlResolution> cacheGet(const std::string& key);
    void cachePut(const std::string& key, SongUrlResolution value);
    std::optional<SongUrlResolution> fetch(const SongRef& ref, int quality,
                                           const std::string& format);

    std::string baseUrl_;  // trailing '/' trimmed
    Transport transport_;
    Clock clock_;
    long long ttlMs_;
    std::size_t maxEntries_;

    std::list<Entry> lru_;  // front = most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
};

// GET /v1/song/url response -> resolution. url is trimmed; empty -> nullopt.
// format falls back to the requested format, then "mp3"
// (online_controller.resolveSongUrl).
std::optional<SongUrlResolution> parseSongUrlResponse(
    std::string_view body, const std::string& requestedFormat);

}  // namespace hemusic
