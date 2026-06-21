#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "net/url_codec.h"

// Codec for the custom `hemusic://` playable-location scheme (PLAN 3.3/3.4).
// A playlist entry stores a stable, self-describing URL; the input_service
// resolves the real stream lazily at play time via /v1/song/url, and get_info
// seeds the playlist row from the embedded hints before /v1/song/detail lands.
//
//   hemusic://song?id=<id>&platform=<platform>
//                 &hint_title=<v>&hint_artist=<v>&hint_album=<v>
//                 &hint_duration=<secs>&hint_cover=<v>
//
// The URL doubles as fb2k's unique track key, so buildSongUrl emits fields in a
// fixed order -- the same SongRef must always render byte-identical. Query
// values are percent-encoded (net/url_codec); only id + platform are mandatory
// (a ref missing either cannot be resolved, so parseSongUrl rejects it). Pure
// logic, header-only, unit-tested in isolation.

namespace hemusic {

struct SongRef {
    std::string id;
    std::string platform;
    std::string hintTitle;
    std::string hintArtist;
    std::string hintAlbum;
    long long hintDuration = 0;  // seconds; 0 == unknown (omitted from the URL)
    std::string hintCover;

    bool operator==(const SongRef& o) const {
        return id == o.id && platform == o.platform &&
               hintTitle == o.hintTitle && hintArtist == o.hintArtist &&
               hintAlbum == o.hintAlbum && hintDuration == o.hintDuration &&
               hintCover == o.hintCover;
    }
};

namespace hemusic_url_detail {

constexpr std::string_view kScheme = "hemusic://";
constexpr std::string_view kSongAuthority = "song";

// Lowercases an ASCII letter only; every other byte is left untouched. (A
// blanket `c | 0x20` would also fold control/punctuation bytes onto letters --
// e.g. 0x1A onto ':' -- and falsely accept a corrupt scheme.)
inline char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII case-insensitive prefix test (the scheme is fixed and always emitted
// lowercase, but a hand-pasted URL may use any casing).
inline bool startsWithCI(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(s.at(i)) != asciiLower(prefix.at(i))) {
            return false;
        }
    }
    return true;
}

// Tolerant non-negative seconds parse; junk/empty -> 0.
inline long long parseSeconds(std::string_view s) {
    long long out = 0;
    const char* begin = s.data();
    const char* end = begin + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return (ec == std::errc{} && ptr == end && out >= 0) ? out : 0;
}

inline void appendParam(std::string& url, char& sep, std::string_view key,
                        std::string_view value) {
    url.push_back(sep);
    url.append(key);
    url.push_back('=');
    url.append(url::percentEncode(value));
    sep = '&';
}

}  // namespace hemusic_url_detail

inline std::string buildSongUrl(const SongRef& ref) {
    namespace d = hemusic_url_detail;
    std::string url(d::kScheme);
    url.append(d::kSongAuthority);
    char sep = '?';
    d::appendParam(url, sep, "id", ref.id);
    d::appendParam(url, sep, "platform", ref.platform);
    if (!ref.hintTitle.empty()) {
        d::appendParam(url, sep, "hint_title", ref.hintTitle);
    }
    if (!ref.hintArtist.empty()) {
        d::appendParam(url, sep, "hint_artist", ref.hintArtist);
    }
    if (!ref.hintAlbum.empty()) {
        d::appendParam(url, sep, "hint_album", ref.hintAlbum);
    }
    if (ref.hintDuration > 0) {
        d::appendParam(url, sep, "hint_duration",
                       std::to_string(ref.hintDuration));
    }
    if (!ref.hintCover.empty()) {
        d::appendParam(url, sep, "hint_cover", ref.hintCover);
    }
    return url;
}

// Parses a hemusic://song URL. Returns nullopt when the scheme/authority is
// wrong or id/platform is missing (an unresolvable ref). Unknown query keys are
// ignored; a value may itself contain '=' (split on the first one only).
inline std::optional<SongRef> parseSongUrl(std::string_view url) {
    namespace d = hemusic_url_detail;
    if (!d::startsWithCI(url, d::kScheme)) {
        return std::nullopt;
    }
    url.remove_prefix(d::kScheme.size());

    std::string_view query;
    if (const auto q = url.find('?'); q != std::string_view::npos) {
        query = url.substr(q + 1);
        url = url.substr(0, q);
    }
    if (!d::startsWithCI(url, d::kSongAuthority) ||
        url.size() != d::kSongAuthority.size()) {
        return std::nullopt;
    }

    SongRef ref;
    while (!query.empty()) {
        std::string_view token;
        if (const auto amp = query.find('&'); amp != std::string_view::npos) {
            token = query.substr(0, amp);
            query = query.substr(amp + 1);
        } else {
            token = query;
            query = {};
        }
        if (token.empty()) {
            continue;
        }
        std::string_view key = token;
        std::string_view rawValue;
        if (const auto eq = token.find('='); eq != std::string_view::npos) {
            key = token.substr(0, eq);
            rawValue = token.substr(eq + 1);
        }
        std::string value = url::percentDecode(rawValue);
        if (key == "id") {
            ref.id = std::move(value);
        } else if (key == "platform") {
            ref.platform = std::move(value);
        } else if (key == "hint_title") {
            ref.hintTitle = std::move(value);
        } else if (key == "hint_artist") {
            ref.hintArtist = std::move(value);
        } else if (key == "hint_album") {
            ref.hintAlbum = std::move(value);
        } else if (key == "hint_duration") {
            ref.hintDuration = d::parseSeconds(value);
        } else if (key == "hint_cover") {
            ref.hintCover = std::move(value);
        }
    }

    if (ref.id.empty() || ref.platform.empty()) {
        return std::nullopt;
    }
    return ref;
}

}  // namespace hemusic
