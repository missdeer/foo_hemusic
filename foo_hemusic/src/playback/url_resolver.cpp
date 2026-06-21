#include "playback/url_resolver.h"

#include <string>
#include <utility>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "net/json_codec.h"
#include "net/url_codec.h"

namespace hemusic {

namespace {

constexpr long kHttpOkMin = 200;
constexpr long kHttpRedirectMin = 300;  // 2xx is [kHttpOkMin, kHttpRedirectMin)
constexpr long kHttpServerErrorMin =
    500;  // >= here is a retryable server error

// id/platform/quality/format -> a cache key. Each component is length-prefixed
// (`<len>:<bytes>`) so the join is injective even when a field carries the
// delimiter: id/platform come from a percent-decoded hemusic:// URL and may
// hold any byte, so a plain separator could let distinct refs collide.
std::string makeKey(const SongRef& ref, int quality,
                    const std::string& format) {
    auto append = [](std::string& key, const std::string& part) {
        key.append(std::to_string(part.size()));
        key.push_back(':');
        key.append(part);
    };
    std::string key;
    append(key, ref.id);
    append(key, ref.platform);
    append(key, std::to_string(quality));
    append(key, format);
    return key;
}

// Flutter `fetchSongUrl`: a blank (empty/whitespace) format becomes "mp3"; a
// non-blank format is used as-is. Normalizing here keeps the request and the
// cache key aligned with the backend instead of fragmenting on "" vs "mp3".
std::string normalizeFormat(const std::string& format) {
    return format.find_first_not_of(" \t\r\n") == std::string::npos ? "mp3"
                                                                    : format;
}

}  // namespace

std::optional<SongUrlResolution> parseSongUrlResponse(
    std::string_view body, const std::string& requestedFormat) {
    boost::system::error_code ec;
    boost::json::value parsed = boost::json::parse(body, ec);
    const boost::json::object obj =
        ec ? boost::json::object{} : json::asObject(parsed);

    std::string url = json::str(obj, "url");
    if (url.empty()) {
        return std::nullopt;
    }
    std::string format = json::str(obj, "format");
    if (format.empty()) {
        format = !requestedFormat.empty() ? requestedFormat : "mp3";
    }
    return SongUrlResolution{std::move(url), std::move(format)};
}

UrlResolver::UrlResolver(std::string baseUrl, Transport transport, Clock clock,
                         long long ttlMs, std::size_t maxEntries)
    : baseUrl_(std::move(baseUrl)),
      transport_(std::move(transport)),
      clock_(std::move(clock)),
      ttlMs_(ttlMs),
      maxEntries_(maxEntries) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') {
        baseUrl_.pop_back();
    }
}

std::optional<SongUrlResolution> UrlResolver::cacheGet(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }
    auto listIt = it->second;
    if (clock_() - listIt->timestampMs <= ttlMs_) {
        lru_.splice(lru_.begin(), lru_, listIt);  // mark most-recently-used
        return listIt->value;
    }
    lru_.erase(listIt);  // expired
    index_.erase(it);
    return std::nullopt;
}

void UrlResolver::cachePut(const std::string& key, SongUrlResolution value) {
    if (auto it = index_.find(key); it != index_.end()) {
        lru_.erase(it->second);
        index_.erase(it);
    }
    lru_.push_front(Entry{key, std::move(value), clock_()});
    index_[key] = lru_.begin();
    if (lru_.size() > maxEntries_) {
        index_.erase(lru_.back().key);
        lru_.pop_back();
    }
}

std::optional<SongUrlResolution> UrlResolver::fetch(const SongRef& ref,
                                                    int quality,
                                                    const std::string& format) {
    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = url::buildUrl(baseUrl_, "/v1/song/url",
                            {{"id", ref.id},
                             {"platform", ref.platform},
                             {"quality", std::to_string(quality)},
                             {"format", format}});
    req.headers.emplace_back("User-Agent", std::string(kHeAudioUserAgent));

    for (int attempt = 1; attempt <= kFetchSongUrlMaxAttempts; ++attempt) {
        HttpResponse resp = transport_(req);
        if (!resp.ok || resp.status >= kHttpServerErrorMin) {
            continue;  // transport error or 5xx -> retry
        }
        if (resp.status < kHttpOkMin || resp.status >= kHttpRedirectMin) {
            return std::nullopt;  // non-2xx (4xx etc.): 401 refresh is
                                  // upstream, 403 captcha is Phase 6 -- not
                                  // retryable here
        }
        if (auto res = parseSongUrlResponse(resp.body, format)) {
            return res;
        }
        // 2xx with a missing url -> retry
    }
    return std::nullopt;
}

std::optional<SongUrlResolution> UrlResolver::resolve(
    const SongRef& ref, int quality, const std::string& format) {
    if (ref.id.empty() || ref.platform.empty()) {
        return std::nullopt;
    }
    const std::string fmt = normalizeFormat(format);
    const std::string key = makeKey(ref, quality, fmt);
    if (auto hit = cacheGet(key)) {
        return hit;
    }
    auto res = fetch(ref, quality, fmt);
    if (!res) {
        return std::nullopt;
    }
    cachePut(key, *res);
    return res;
}

void UrlResolver::invalidate(const SongRef& ref, int quality,
                             const std::string& format) {
    const std::string key = makeKey(ref, quality, normalizeFormat(format));
    if (auto it = index_.find(key); it != index_.end()) {
        lru_.erase(it->second);
        index_.erase(it);
    }
}

void UrlResolver::clear() {
    lru_.clear();
    index_.clear();
}

}  // namespace hemusic
