#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/platforms.h"
#include "api/song.h"
#include "net/json_codec.h"

// Radio endpoint models + parsers, ported from HE-Music-Flutter
// `features/radio/data/datasources/radio_api_client.dart` and the shared
// RadioInfo / RadioGroupInfo models. Two endpoints:
//   GET /v1/radios       -> { groups: [RadioGroupInfo] }
//   GET /v1/radio/songs  -> { list:   [SongInfo] }
// Header-only; HTTP wiring (via ApiClient) is added with the Phase 5 radio
// page.
//
// Lenient divergence (consistent with discover.h / platforms.h): Flutter throws
// on a missing/non-array `groups` or `list`; the pure parser degrades to an
// empty vector instead.

namespace hemusic {

struct RadioInfo {
    std::string name;
    std::string id;
    std::string cover;
    std::string platform;
};

struct RadioGroupInfo {
    std::string name;
    std::vector<RadioInfo> radios;
    std::string platform;
};

// `RadioInfo.fromMap`.
inline RadioInfo parseRadioInfo(const boost::json::value& value,
                                std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    RadioInfo r;
    r.name = json::str(o, "name");
    r.id = json::str(o, "id");
    r.cover = model_detail::cover(o);
    std::string platform = json::str(o, "platform");
    r.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    return r;
}

// `_radios`: array only; entries with an empty id or name are dropped.
inline std::vector<RadioInfo> parseRadioList(
    const boost::json::value& value, std::string_view fallbackPlatform = "") {
    std::vector<RadioInfo> out;
    if (!value.is_array()) {
        return out;
    }
    for (const auto& item : value.get_array()) {
        RadioInfo r = parseRadioInfo(item, fallbackPlatform);
        if (!r.id.empty() && !r.name.empty()) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

// `RadioGroupInfo.fromMap`. Children inherit the group's resolved platform as
// their fallback (matching `_radios(raw['radios'], platform)`).
inline RadioGroupInfo parseRadioGroupInfo(
    const boost::json::value& value, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    RadioGroupInfo g;
    std::string platform = json::str(o, "platform");
    g.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    g.name = json::str(o, "name");
    if (const auto* radios = o.if_contains("radios")) {
        g.radios = parseRadioList(*radios, g.platform);
    }
    return g;
}

// `/v1/radios` -> { groups: [...] }. Missing/non-array groups -> empty.
inline std::vector<RadioGroupInfo> parseRadioGroups(
    const boost::json::value& body, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    std::vector<RadioGroupInfo> out;
    const auto* groups = o.if_contains("groups");
    if (groups == nullptr || !groups->is_array()) {
        return out;
    }
    for (const auto& item : groups->get_array()) {
        if (!item.is_object()) {
            continue;  // skip non-object entries (lenient: Flutter _asMap
                       // throws)
        }
        out.push_back(parseRadioGroupInfo(item, fallbackPlatform));
    }
    return out;
}

// PlatformFeatureSupportFlag.listRadios from Flutter
// `features/online/domain/entities/online_platform.dart`. Mirrors discover.h's
// kFeatureGetDiscoverPage layout (1ULL bit-shift; uint64 fits the 1<<47 max).
inline constexpr unsigned long long kFeatureListRadios = 1ULL << 39;

// First available platform that lists radios. Mirrors
// resolveDiscoverPlatform: degrades to nullopt instead of throwing when no
// platform qualifies (Flutter throws StateError; caller renders empty state).
inline std::optional<PlatformInfo> resolveRadioPlatform(
    const std::vector<PlatformInfo>& platforms) {
    for (const auto& p : platforms) {
        if (p.available() && p.supports(kFeatureListRadios)) {
            return p;
        }
    }
    return std::nullopt;
}

// `/v1/radio/songs` -> { list: [...] }. Reuses parseSongList, which drops songs
// with an empty id/name (unplayable). This is a deliberate divergence from the
// Flutter radio fetchSongs (which keeps all), matching our player-safety stance
// in parseSongList / parsePlaylistSongs.
inline std::vector<SongInfo> parseRadioSongs(
    const boost::json::value& body, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    const auto* list = o.if_contains("list");
    if (list == nullptr) {
        return {};
    }
    return parseSongList(*list, fallbackPlatform);
}

}  // namespace hemusic
