#include "ui/cover_cache.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/http_client.h"
#include "ui/image_cache.h"

namespace hemusic::ui {

namespace {

// Cover URLs are direct CDN links (api.md sec.7.5): plain GET, no bearer, no
// interceptor chain. Short timeouts so shutdownCoverCache()'s join doesn't sit
// on a stuck socket for the default 20s/30s.
constexpr int kCoverConnectMs = 5000;
constexpr int kCoverReadMs = 8000;
constexpr long kHttpOkMin = 200;
constexpr long kHttpOkMax = 300;  // exclusive

std::vector<std::uint8_t> fetchCover(const std::string& url) {
    HttpClient http;
    HttpRequest req;
    req.url = url;
    req.connectTimeoutMs = kCoverConnectMs;
    req.readTimeoutMs = kCoverReadMs;
    const HttpResponse resp = http.send(req);
    if (!resp.ok || resp.status < kHttpOkMin || resp.status >= kHttpOkMax) {
        return {};  // empty bytes -> ImageCache marks the entry Failed
    }
    return {resp.body.begin(), resp.body.end()};
}

// Owned by the component initquit (see header): a namespace-scope global, not a
// function-local static. on_quit resets it, so its destruction at DLL unload
// sees an empty unique_ptr and never joins worker threads under the loader
// lock.
std::unique_ptr<ImageCache> g_cache;

}  // namespace

void initCoverCache() {
    if (!g_cache) {
        g_cache = std::make_unique<ImageCache>(&fetchCover);
    }
}

void shutdownCoverCache() {
    // Resets the unique_ptr -> ImageCache dtor stops + joins the workers here
    // on the main thread, while the DLL is still loaded.
    g_cache.reset();
}

ImageCache* coverCache() { return g_cache.get(); }

}  // namespace hemusic::ui
