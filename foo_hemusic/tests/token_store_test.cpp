#include "auth/token_store.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "auth/oauth_flow.h"

using hemusic::AuthTokenResult;
using hemusic::deserializeToken;
using hemusic::dpapiProtect;
using hemusic::dpapiUnprotect;
using hemusic::serializeToken;
using hemusic::TokenStore;

namespace {

constexpr long long kSampleExpiresAt = 1750000000;

AuthTokenResult sampleToken() {
    return AuthTokenResult{"access-XYZ", "refresh-ABC", kSampleExpiresAt};
}

// Unique temp path per test; removed by the destructor.
struct TempPath {
    std::filesystem::path path;
    TempPath() {
        static std::atomic<unsigned> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("foo_hemusic_tok_" + std::to_string(counter.fetch_add(1)) +
                ".bin");
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    TempPath(TempPath&&) = delete;
    TempPath& operator=(TempPath&&) = delete;
};

std::vector<unsigned char> readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("serializeToken round-trips the credential triple", "[token]") {
    auto json = serializeToken(sampleToken());
    auto back = deserializeToken(json);

    REQUIRE(back.has_value());
    REQUIRE(back->accessToken == "access-XYZ");
    REQUIRE(back->refreshToken == "refresh-ABC");
    REQUIRE(back->expiresAt == 1750000000);
}

TEST_CASE("deserializeToken rejects junk and tokenless payloads", "[token]") {
    REQUIRE_FALSE(deserializeToken("not json").has_value());
    REQUIRE_FALSE(deserializeToken("[]").has_value());
    // A blank access_token is not a usable credential.
    REQUIRE_FALSE(deserializeToken(R"({"access_token":"","refresh_token":"r"})")
                      .has_value());
}

TEST_CASE("dpapi protect/unprotect is a faithful round-trip", "[token]") {
    // Embedded NUL proves the round-trip is binary-safe, not string-terminated.
    const std::vector<unsigned char> plain{'s', 'e', 'c', 0, 'r', 'e', 't'};
    auto cipher = dpapiProtect(plain);
    REQUIRE(cipher.has_value());
    // Ciphertext must differ from plaintext -- otherwise it isn't encrypted.
    REQUIRE(*cipher != plain);

    auto back = dpapiUnprotect(*cipher);
    REQUIRE(back.has_value());
    REQUIRE(*back == plain);
}

TEST_CASE("dpapiUnprotect fails on non-DPAPI bytes", "[token]") {
    constexpr std::size_t kGarbageLen = 64;
    constexpr unsigned char kGarbageByte = 0xAB;
    const std::vector<unsigned char> garbage(kGarbageLen, kGarbageByte);
    REQUIRE_FALSE(dpapiUnprotect(garbage).has_value());
}

TEST_CASE("TokenStore save/load round-trips and creates parent dirs",
          "[token]") {
    TempPath tmp;
    // Nest under a missing subdir to exercise create_directories.
    auto nested =
        tmp.path.parent_path() / "foo_hemusic_sub" / tmp.path.filename();
    TokenStore store(nested);

    REQUIRE(store.save(sampleToken()));
    auto loaded = store.load();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->accessToken == "access-XYZ");
    REQUIRE(loaded->refreshToken == "refresh-ABC");
    REQUIRE(loaded->expiresAt == 1750000000);

    REQUIRE(store.clear());
    REQUIRE_FALSE(store.load().has_value());
    std::error_code ec;
    std::filesystem::remove(nested.parent_path(), ec);
}

// Atomic write: overwriting an existing credential must succeed and leave no
// stray ".tmp" sibling behind (the temp file is renamed into place, not kept).
TEST_CASE("TokenStore save overwrites in place without temp residue",
          "[token]") {
    TempPath tmp;
    TokenStore store(tmp.path);
    REQUIRE(store.save(sampleToken()));
    REQUIRE(store.save(AuthTokenResult{"access-2", "refresh-2", 1}));

    auto loaded = store.load();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->accessToken == "access-2");  // second write won

    auto residue = tmp.path;
    residue += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(residue));
    std::error_code ec;
    std::filesystem::remove(residue, ec);
}

// The whole point of DPAPI here: the token must not sit on disk in plaintext.
TEST_CASE("TokenStore writes ciphertext, never the plaintext token",
          "[token]") {
    TempPath tmp;
    TokenStore store(tmp.path);
    REQUIRE(store.save(sampleToken()));

    auto bytes = readAll(tmp.path);
    std::string onDisk(bytes.begin(), bytes.end());
    REQUIRE(onDisk.find("access-XYZ") == std::string::npos);
    REQUIRE(onDisk.find("refresh-ABC") == std::string::npos);
}

TEST_CASE("TokenStore load returns nullopt when the file is absent",
          "[token]") {
    TempPath tmp;  // never written
    TokenStore store(tmp.path);
    REQUIRE_FALSE(store.load().has_value());
    // clear() on a non-existent file is a successful no-op.
    REQUIRE(store.clear());
}

TEST_CASE("TokenStore load returns nullopt on corrupt (non-DPAPI) file",
          "[token]") {
    TempPath tmp;
    {
        std::ofstream out(tmp.path, std::ios::binary);
        const char junk[] = "not encrypted by dpapi";
        out.write(junk, sizeof(junk));
    }
    TokenStore store(tmp.path);
    REQUIRE_FALSE(store.load().has_value());
}
