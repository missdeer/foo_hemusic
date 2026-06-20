#include "net/http_client.h"

#include <catch2/catch_test_macros.hpp>

using hemusic::crackUrl;

// crackUrl is the URL splitter every request funnels through before WinHTTP;
// the whole api/ layer trusts it to surface host/port/path correctly. These
// pin the contract WinHttpCrackUrl gives us (scheme default ports, path+query
// concatenation) so a refactor that drops the query or mishandles the default
// port fails loudly rather than silently hitting the wrong endpoint.

TEST_CASE("crackUrl splits https url, keeping path and query", "[http]") {
    auto u = crackUrl("https://api.example.com/v1/auth/status?state=abc");

    REQUIRE(u.has_value());
    CHECK(u->https);
    CHECK(u->host == L"api.example.com");
    CHECK(u->port == 443);  // scheme default, not present in the URL
    CHECK(u->path == L"/v1/auth/status?state=abc");
}

TEST_CASE("crackUrl fills the http default port and root path", "[http]") {
    auto u = crackUrl("http://example.com");

    REQUIRE(u.has_value());
    CHECK_FALSE(u->https);
    CHECK(u->port == 80);  // scheme default
    CHECK(u->path == L"/");
}

TEST_CASE("crackUrl honors an explicit port", "[http]") {
    auto u = crackUrl("https://host.test:8443/path");

    REQUIRE(u.has_value());
    CHECK(u->port == 8443);
    CHECK(u->path == L"/path");
}

TEST_CASE("crackUrl rejects input without a scheme", "[http]") {
    CHECK_FALSE(crackUrl("not a url").has_value());
    CHECK_FALSE(crackUrl("").has_value());
    CHECK_FALSE(crackUrl("example.com/path").has_value());
}
