#include "playback/url_resolver.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "net/http_client.h"
#include "playback/hemusic_url.h"

using hemusic::HttpRequest;
using hemusic::HttpResponse;
using hemusic::kHeAudioUserAgent;
using hemusic::parseSongUrlResponse;
using hemusic::SongRef;
using hemusic::UrlResolver;

namespace {

// Returns programmed responses in order, repeating the last one; records each
// request and the call count. Captured by reference into the resolver's
// std::function transport so the counters stay live.
struct FakeTransport {
    std::vector<HttpResponse> responses;
    int calls = 0;
    std::vector<HttpRequest> seen;

    HttpResponse operator()(const HttpRequest& req) {
        seen.push_back(req);
        const std::size_t idx = std::min<std::size_t>(
            static_cast<std::size_t>(calls), responses.size() - 1);
        ++calls;
        return responses.at(idx);
    }
};

HttpResponse ok(const std::string& body) {
    return HttpResponse{true, 200, body, 0, ""};
}
HttpResponse serverError() { return HttpResponse{true, 500, "", 0, ""}; }
HttpResponse notFound() { return HttpResponse{true, 404, "", 0, ""}; }
HttpResponse transportError() {
    return HttpResponse{false, 0, "", 12029, "cannot connect"};
}

const std::string kBody = R"({"url":"http://s/a.mp3","format":"mp3"})";

SongRef song(std::string id, std::string platform) {
    SongRef r;
    r.id = std::move(id);
    r.platform = std::move(platform);
    return r;
}

bool hasHeader(const HttpRequest& req, std::string_view name,
               std::string_view value) {
    return std::any_of(
        req.headers.begin(), req.headers.end(),
        [&](const auto& h) { return h.first == name && h.second == value; });
}

}  // namespace

TEST_CASE("resolve issues a GET with id/platform/quality/format + audio UA") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test/", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });

    auto res = r.resolve(song("s1", "qq"));
    REQUIRE(res.has_value());
    CHECK(res->url == "http://s/a.mp3");
    CHECK(res->format == "mp3");

    REQUIRE(t.calls == 1);
    const auto& req = t.seen.at(0);
    CHECK(req.url ==
          "https://api.test/v1/song/"
          "url?id=s1&platform=qq&quality=320&format=mp3");
    CHECK(hasHeader(req, "User-Agent", kHeAudioUserAgent));
}

TEST_CASE("a fresh cache hit is served without a second request") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    long long now = 0;
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [&] { return now; });

    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    now = 30000;  // exactly TTL -> still fresh (<=)
    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    CHECK(t.calls == 1);
}

TEST_CASE("an expired entry is refetched") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    long long now = 0;
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [&] { return now; });

    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    now = 30001;  // one ms past TTL
    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    CHECK(t.calls == 2);
}

TEST_CASE("invalidate forces the next resolve to refetch") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });

    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    r.invalidate(song("s1", "qq"));
    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    CHECK(t.calls == 2);
}

TEST_CASE("quality is part of the cache key") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });

    REQUIRE(r.resolve(song("s1", "qq"), 320).has_value());
    REQUIRE(r.resolve(song("s1", "qq"), 999).has_value());  // different quality
    CHECK(t.calls == 2);
}

TEST_CASE("the cache key is injective when a field carries the delimiter") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });

    // Under a plain "id\x1fplatform" join these two collide; length-prefixing
    // keeps them distinct, so each must trigger its own fetch.
    REQUIRE(r.resolve(song(std::string("a\x1f") + "b", "c")).has_value());
    REQUIRE(r.resolve(song("a", std::string("b\x1f") + "c")).has_value());
    CHECK(t.calls == 2);
}

TEST_CASE("a blank format is normalized to mp3 (request + cache key)") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });

    REQUIRE(r.resolve(song("s1", "qq"), 320, "  ").has_value());  // whitespace
    CHECK(t.seen.at(0).url.find("format=mp3") != std::string::npos);
    // The blank-format and explicit-mp3 calls must hit the same cache entry.
    REQUIRE(r.resolve(song("s1", "qq"), 320, "mp3").has_value());
    CHECK(t.calls == 1);
}

TEST_CASE("a server error is retried up to the attempt cap") {
    SECTION("recovers on a later attempt") {
        FakeTransport t;
        t.responses = {serverError(), ok(kBody)};
        UrlResolver r(
            "https://api.test", [&](const HttpRequest& q) { return t(q); },
            [] { return 0LL; });
        REQUIRE(r.resolve(song("s1", "qq")).has_value());
        CHECK(t.calls == 2);
    }
    SECTION("gives up after 3 server errors") {
        FakeTransport t;
        t.responses = {serverError()};
        UrlResolver r(
            "https://api.test", [&](const HttpRequest& q) { return t(q); },
            [] { return 0LL; });
        CHECK_FALSE(r.resolve(song("s1", "qq")).has_value());
        CHECK(t.calls == 3);
        CHECK(r.size() == 0);  // a failed resolve is not cached
    }
}

TEST_CASE("a transport error is retryable") {
    FakeTransport t;
    t.responses = {transportError(), transportError(), ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });
    REQUIRE(r.resolve(song("s1", "qq")).has_value());
    CHECK(t.calls == 3);
}

TEST_CASE("a 4xx fails immediately without retry") {
    FakeTransport t;
    t.responses = {notFound()};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });
    CHECK_FALSE(r.resolve(song("s1", "qq")).has_value());
    CHECK(t.calls == 1);  // 401 refresh is upstream; 403 captcha is Phase 6
}

TEST_CASE("a 2xx with a missing url is retried then fails") {
    FakeTransport t;
    t.responses = {ok(R"({"format":"mp3"})")};  // no url
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });
    CHECK_FALSE(r.resolve(song("s1", "qq")).has_value());
    CHECK(t.calls == 3);
}

TEST_CASE("resolve rejects an empty id or platform without any request") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; });
    CHECK_FALSE(r.resolve(song("", "qq")).has_value());
    CHECK_FALSE(r.resolve(song("s1", "")).has_value());
    CHECK(t.calls == 0);
}

TEST_CASE("the LRU evicts the least-recently-used entry past capacity") {
    FakeTransport t;
    t.responses = {ok(kBody)};
    UrlResolver r(
        "https://api.test", [&](const HttpRequest& q) { return t(q); },
        [] { return 0LL; }, hemusic::kDefaultUrlTtlMs,
        /*maxEntries=*/2);

    REQUIRE(r.resolve(song("a", "qq")).has_value());
    REQUIRE(r.resolve(song("b", "qq")).has_value());
    REQUIRE(r.resolve(song("c", "qq")).has_value());  // evicts "a"
    CHECK(r.size() == 2);
    CHECK(t.calls == 3);

    REQUIRE(r.resolve(song("b", "qq")).has_value());  // still cached
    CHECK(t.calls == 3);
    REQUIRE(r.resolve(song("a", "qq")).has_value());  // evicted -> refetch
    CHECK(t.calls == 4);
}

TEST_CASE("parseSongUrlResponse trims url and falls back for format") {
    SECTION("trims url, uses response format") {
        auto res = parseSongUrlResponse(
            R"({"url":"  http://x  ","format":"flac"})", "mp3");
        REQUIRE(res.has_value());
        CHECK(res->url == "http://x");
        CHECK(res->format == "flac");
    }
    SECTION("missing url -> nullopt") {
        CHECK_FALSE(
            parseSongUrlResponse(R"({"format":"flac"})", "mp3").has_value());
    }
    SECTION("missing format -> requested format") {
        auto res = parseSongUrlResponse(R"({"url":"http://x"})", "flac");
        REQUIRE(res.has_value());
        CHECK(res->format == "flac");
    }
    SECTION("missing format and no requested -> mp3") {
        auto res = parseSongUrlResponse(R"({"url":"http://x"})", "");
        REQUIRE(res.has_value());
        CHECK(res->format == "mp3");
    }
    SECTION("non-JSON body -> nullopt") {
        CHECK_FALSE(parseSongUrlResponse("<html>oops", "mp3").has_value());
    }
}
