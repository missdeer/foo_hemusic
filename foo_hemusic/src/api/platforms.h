#pragma once

#include <charconv>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "net/json_codec.h"

// GET /v1/platforms response model + parsers, ported from HE-Music-Flutter
// `features/home/domain/entities/home_platform.dart` (HomePlatform.fromMap).
// The platform list drives the platform-selection tabs (discover / ranking /
// search) and per-platform capability gating via featureSupportFlag. This is
// the lean home/discover view of a platform; OnlinePlatform additionally
// carries imageSizes/qualities for search/detail and is ported when its Phase 5
// consumer lands. Header-only like the rest of api/.
//
// Envelope: { "list": [ {platform}, ... ] }. fromMap throws on an entry with an
// empty id or name; the pure parser instead drops it (same lenient stance as
// parseSongList), so parsePlatformList yields only renderable/selectable
// platforms.

namespace hemusic {

struct PlatformInfo {
    std::string id;
    std::string name;
    std::string shortName;
    long long status = 0;
    unsigned long long featureSupportFlag = 0;

    bool available() const { return status == 1; }
    bool supports(unsigned long long flag) const {
        return (featureSupportFlag & flag) != 0;
    }
};

namespace platform_detail {

// `_readFeatureSupportFlag`: numeric value as-is; a numeric string is parsed
// (full match, else 0); anything else -> 0. Unsigned because it is a bitmask;
// the defined flags top out at 1<<47, well within uint64.
inline unsigned long long featureFlag(const boost::json::value* v) {
    if (v == nullptr) {
        return 0;
    }
    switch (v->kind()) {
        case boost::json::kind::int64: {
            long long n = v->get_int64();
            return n < 0 ? 0 : static_cast<unsigned long long>(n);
        }
        case boost::json::kind::uint64:
            return v->get_uint64();
        case boost::json::kind::double_:
            // Flutter stringifies a double and BigInt.tryParse fails on it ->
            // 0; returning 0 also avoids UB from casting a huge/NaN double to
            // ull.
            return 0;
        case boost::json::kind::string: {
            const auto& s = v->get_string();
            unsigned long long out = 0;
            const char* begin = s.c_str();
            const char* end = begin + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, out);
            return (ec == std::errc{} && ptr == end) ? out : 0;
        }
        default:
            return 0;
    }
}

}  // namespace platform_detail

// `HomePlatform.fromMap`. shortName falls back to name when `shortname` is
// empty/absent. A non-object yields an all-empty PlatformInfo (id/name empty),
// which parsePlatformList drops.
inline PlatformInfo parsePlatformInfo(const boost::json::value& value) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    PlatformInfo p;
    p.id = json::str(o, "id");
    p.name = json::str(o, "name");
    std::string shortName = json::str(o, "shortname");
    p.shortName = shortName.empty() ? p.name : std::move(shortName);
    p.status = json::i64(o, "status");
    p.featureSupportFlag =
        platform_detail::featureFlag(o.if_contains("feature_support_flag"));
    return p;
}

// Parses the `{ "list": [...] }` envelope; platforms with an empty id or name
// are dropped (fromMap throws on them, the lenient equivalent is to skip).
// A missing/non-array `list` yields an empty vector.
inline std::vector<PlatformInfo> parsePlatformList(
    const boost::json::value& body) {
    std::vector<PlatformInfo> out;
    const auto obj = json::asObject(body);
    const auto* list = obj.if_contains("list");
    if (list == nullptr || !list->is_array()) {
        return out;
    }
    const auto& arr = list->get_array();
    out.reserve(arr.size());
    for (const auto& item : arr) {
        PlatformInfo p = parsePlatformInfo(item);
        if (!p.id.empty() && !p.name.empty()) {
            out.push_back(std::move(p));
        }
    }
    return out;
}

}  // namespace hemusic
