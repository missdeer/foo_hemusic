#include "playback/hemusic_url.h"

#include <catch2/catch_test_macros.hpp>

using hemusic::buildSongUrl;
using hemusic::parseSongUrl;
using hemusic::SongRef;

namespace {
constexpr long long kSampleDuration = 215;  // matches "hint_duration=215"
constexpr long long kNegativeDuration = -5;
constexpr long long kRoundTripDuration = 180;
}  // namespace

TEST_CASE("buildSongUrl emits id+platform in fixed order, omits empty hints") {
    SongRef ref;
    ref.id = "s1";
    ref.platform = "netease";
    CHECK(buildSongUrl(ref) == "hemusic://song?id=s1&platform=netease");
}

TEST_CASE("buildSongUrl encodes hints and keeps a deterministic field order") {
    SongRef ref;
    ref.id = "s1";
    ref.platform = "qq";
    ref.hintTitle = "Hello World";
    ref.hintArtist = "A & B";
    ref.hintAlbum = "Best=Of";
    ref.hintDuration = kSampleDuration;
    ref.hintCover = "http://x/c.jpg";
    CHECK(buildSongUrl(ref) ==
          "hemusic://song?id=s1&platform=qq&hint_title=Hello%20World"
          "&hint_artist=A%20%26%20B&hint_album=Best%3DOf&hint_duration=215"
          "&hint_cover=http%3A%2F%2Fx%2Fc.jpg");
}

TEST_CASE("buildSongUrl omits a zero/negative duration") {
    SongRef ref;
    ref.id = "s1";
    ref.platform = "qq";
    ref.hintDuration = 0;
    CHECK(buildSongUrl(ref).find("hint_duration") == std::string::npos);
    ref.hintDuration = kNegativeDuration;
    CHECK(buildSongUrl(ref).find("hint_duration") == std::string::npos);
}

TEST_CASE("parse round-trips a fully populated ref including tricky chars") {
    SongRef ref;
    ref.id = "id with space";
    ref.platform = "ne&t=x";
    ref.hintTitle = "T?&=%";
    ref.hintArtist =
        "\xE4\xBD\xA0 artist";  // raw UTF-8 bytes, no /utf-8 needed
    ref.hintAlbum = "";         // omitted, stays empty on parse
    ref.hintDuration = kRoundTripDuration;
    ref.hintCover = "https://c/d?e=f&g=h";

    auto parsed = parseSongUrl(buildSongUrl(ref));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == ref);
}

TEST_CASE("parseSongUrl rejects a non-hemusic scheme") {
    CHECK_FALSE(parseSongUrl("http://song?id=s1&platform=qq").has_value());
    CHECK_FALSE(parseSongUrl("song?id=s1&platform=qq").has_value());
}

TEST_CASE("case-insensitive match only folds ASCII letters, not punctuation") {
    // 0x1A | 0x20 == ':' (0x3A); a blanket-OR tolower would accept this corrupt
    // scheme where the ':' should be. asciiLower must reject it.
    const std::string corrupt =
        std::string("hemusic\x1a") + "//song?id=s1&platform=qq";
    CHECK_FALSE(parseSongUrl(corrupt).has_value());
}

TEST_CASE("parseSongUrl rejects a non-song authority") {
    CHECK_FALSE(parseSongUrl("hemusic://album?id=a1&platform=qq").has_value());
    // "song" must match exactly, not as a prefix.
    CHECK_FALSE(parseSongUrl("hemusic://songs?id=s1&platform=qq").has_value());
}

TEST_CASE("parseSongUrl accepts a case-insensitive scheme") {
    auto parsed = parseSongUrl("HEMUSIC://song?id=s1&platform=qq");
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "s1");
    CHECK(parsed->platform == "qq");
}

TEST_CASE("parseSongUrl requires both id and platform") {
    CHECK_FALSE(parseSongUrl("hemusic://song?platform=qq").has_value());
    CHECK_FALSE(parseSongUrl("hemusic://song?id=s1").has_value());
    CHECK_FALSE(parseSongUrl("hemusic://song").has_value());
    CHECK_FALSE(parseSongUrl("hemusic://song?id=&platform=qq").has_value());
}

TEST_CASE("parseSongUrl tolerates junk duration and ignores unknown keys") {
    auto parsed = parseSongUrl(
        "hemusic://song?id=s1&platform=qq&hint_duration=abc&foo=bar&extra");
    REQUIRE(parsed.has_value());
    CHECK(parsed->hintDuration == 0);
    CHECK(parsed->hintTitle.empty());
}

TEST_CASE("parseSongUrl keeps '=' inside a hand-pasted value") {
    // A raw (unencoded) '=' in a value: split on the first '=' only.
    auto parsed =
        parseSongUrl("hemusic://song?id=s1&platform=qq&hint_title=a=b=c");
    REQUIRE(parsed.has_value());
    CHECK(parsed->hintTitle == "a=b=c");
}

TEST_CASE("parseSongUrl skips empty query tokens") {
    auto parsed = parseSongUrl("hemusic://song?&id=s1&&platform=qq&");
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "s1");
    CHECK(parsed->platform == "qq");
}
