#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RFC 3986 percent-encoding for URL query components. Shared by the auth URL
// builder (auth/login_flow) and the hemusic:// scheme codec (playback). Space
// is encoded as %20 (not '+'), so the decoder leaves a literal '+' untouched --
// both ends are ours, the encoding is unambiguous. Header-only like json_codec.

namespace hemusic::url {

// Keeps RFC 3986 unreserved (A-Z a-z 0-9 - _ . ~) verbatim; everything else is
// %XX with uppercase hex.
inline std::string percentEncode(std::string_view value) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    constexpr unsigned kNibbleMask = 0x0F;
    constexpr int kNibbleShift = 4;
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z') ||
                                (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' ||
                                c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex.at(c >> kNibbleShift));
            out.push_back(kHex.at(c & kNibbleMask));
        }
    }
    return out;
}

namespace detail {

// Hex digit value, or -1 if not a hex digit.
inline int hexVal(char c) {
    constexpr int kHexLetterBase = 10;  // value of 'a'/'A'
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + kHexLetterBase;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + kHexLetterBase;
    }
    return -1;
}

}  // namespace detail

// Inverse of percentEncode: "%XX" -> byte. A '%' not followed by two hex digits
// is emitted literally (lenient; we never produce such input). Bytes pass
// through untouched, so UTF-8 sequences survive a round trip.
inline std::string percentDecode(std::string_view value) {
    constexpr int kNibbleShift = 4;
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value.at(i);
        if (c == '%' && i + 2 < value.size()) {
            const int hi = detail::hexVal(value.at(i + 1));
            const int lo = detail::hexVal(value.at(i + 2));
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << kNibbleShift) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

// Joins baseUrl (trailing '/' trimmed) + path + a percent-encoded query string.
// Both key and value are encoded. Used by the auth URL builder and the
// /v1/song/url resolver.
inline std::string buildUrl(
    std::string_view baseUrl, std::string_view path,
    const std::vector<std::pair<std::string, std::string>>& query = {}) {
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.remove_suffix(1);
    }
    std::string out(baseUrl);
    out.append(path);
    char sep = '?';
    for (const auto& [key, val] : query) {
        out.push_back(sep);
        out.append(percentEncode(key));
        out.push_back('=');
        out.append(percentEncode(val));
        sep = '&';
    }
    return out;
}

}  // namespace hemusic::url
