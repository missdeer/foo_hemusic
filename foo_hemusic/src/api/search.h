#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <boost/json.hpp>

#include "api/album.h"
#include "api/artist.h"
#include "api/discover.h"  // reuses discover_detail::mapAll
#include "api/mv.h"
#include "api/platforms.h"
#include "api/playlist.h"
#include "api/song.h"
#include "net/json_codec.h"

// Search endpoint models + parsers, ported from HE-Music-Flutter
// `features/online/data/online_api_client.dart` (comprehensiveSearch /
// searchMusic) and `presentation/pages/online_search_models.dart`. Two shapes:
//   GET /v1/search          -> comprehensive result (best match + 5 sections)
//   GET /v1/{type}/search   -> a single typed list ({ list: [...] })
// Header-only; HTTP wiring added with the Phase 5 search page.
//
// The Flutter comprehensive result stores RAW maps per section and types them
// lazily at display via `searchXInfo(item)`. Here each section is eagerly typed
// (the section's element type is fixed: song/playlist/album/mv/artist), which
// is equivalent and more natural in C++. Sections and best-match items use the
// REQUEST platform as the model fallback -- a deliberate, documented divergence
// from Flutter's `_safePlatform` (which returns "-" for a platform-less item,
// only ever fed display strings); the request platform yields a usable platform
// key for playback instead of an unplayable "-".
//
// Item mapping reuses discover_detail::mapAll: every object array entry is kept
// (no id/name filter), non-object entries skipped -- matching Flutter
// searchMusic / `_extractList`, which return the raw list unfiltered.

namespace hemusic {

// PlatformFeatureSupportFlag.comprehensiveSearch (online_platform.dart: 1 <<
// 0). The flag the Flutter comprehensive search gates on
// (online_search_models.dart `SearchType.comprehensive ->
// comprehensiveSearch`). The per-type search flags (searchSong/Album/... are
// 1<<1..1<<5) gate the category sub-tabs, a follow-up.
inline constexpr unsigned long long kFeatureComprehensiveSearch = 1ULL << 0;

// Picks the platform the comprehensive search runs against: the first platform
// that is available() AND supports comprehensiveSearch, mirroring
// resolveDiscoverPlatform (discover.h). nullopt when none qualifies (caller
// renders an error state). The per-platform search sub-tabs (kuwo / netease /
// ...) are a follow-up; the skeleton always uses this default.
inline std::optional<PlatformInfo> resolveSearchPlatform(
    const std::vector<PlatformInfo>& platforms) {
    for (const auto& p : platforms) {
        if (p.available() && p.supports(kFeatureComprehensiveSearch)) {
            return p;
        }
    }
    return std::nullopt;
}

template <typename T>
struct SearchSection {
    std::vector<T> items;
    bool hasMore = false;
    long long totalCount = 0;
};

using BestMatchData =
    std::variant<SongInfo, PlaylistInfo, AlbumInfo, MvInfo, ArtistInfo>;

struct BestMatchItem {
    std::string resourceType;  // song | artist | playlist | album | mv
    BestMatchData data;
};

struct ComprehensiveSearchResult {
    std::string keyword;
    std::vector<BestMatchItem> bestMatch;
    SearchSection<SongInfo> song;
    SearchSection<PlaylistInfo> playlist;
    SearchSection<AlbumInfo> album;
    SearchSection<MvInfo> video;
    SearchSection<ArtistInfo> artist;
};

namespace search_detail {

// `_readField`: payload[field], falling back to payload["data"][field].
inline const boost::json::value* readField(const boost::json::object& o,
                                           const char* field) {
    if (const auto* v = o.if_contains(field)) {
        return v;
    }
    if (const auto* d = o.if_contains("data"); d != nullptr && d->is_object()) {
        if (const auto* v = d->get_object().if_contains(field)) {
            return v;
        }
    }
    return nullptr;
}

// `_readBoolField`: bool -> bool; number -> > 0; "true"/"1" -> true;
// "false"/"0" -> false; otherwise false.
inline bool readBoolField(const boost::json::object& o, const char* field) {
    const auto* v = readField(o, field);
    if (v == nullptr) {
        return false;
    }
    if (v->is_bool()) {
        return v->get_bool();
    }
    if (v->is_int64() || v->is_uint64() || v->is_double()) {
        return json::toI64(*v) > 0;
    }
    std::string s = json::toStr(*v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "true" || s == "1";
}

// `_readIntField`: int/double -> value (truncated); numeric string -> parsed;
// otherwise 0.
inline long long readIntField(const boost::json::object& o, const char* field) {
    const auto* v = readField(o, field);
    return v != nullptr ? json::toI64(*v) : 0;
}

// `_extractList`: first array among list/items/data, or the first such array
// nested one level inside one of those keys; nullptr if none.
inline const boost::json::value* sectionList(const boost::json::object& o) {
    static constexpr const char* kKeys[] = {"list", "items", "data"};
    for (const char* k : kKeys) {
        const auto* v = o.if_contains(k);
        if (v == nullptr) {
            continue;
        }
        if (v->is_array()) {
            return v;
        }
        if (v->is_object()) {
            const auto& child = v->get_object();
            for (const char* ck : kKeys) {
                const auto* nv = child.if_contains(ck);
                if (nv != nullptr && nv->is_array()) {
                    return nv;
                }
            }
        }
    }
    return nullptr;
}

// `_readComprehensiveSection`: typed items + has_more + total_count.
template <typename T>
SearchSection<T> parseSection(const boost::json::value* sectionVal,
                              std::string_view platform,
                              T (*parse)(const boost::json::value&,
                                         std::string_view)) {
    SearchSection<T> section;
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        (sectionVal != nullptr && sectionVal->is_object())
            ? sectionVal->get_object()
            : kEmpty;
    section.items = discover_detail::mapAll(sectionList(o), platform, parse);
    section.hasMore = readBoolField(o, "has_more");
    section.totalCount = readIntField(o, "total_count");
    return section;
}

// `_readRecommendItem`: { resourceType, <resourceType>: {data} } -> typed item.
// Empty resourceType / empty data / unknown type -> nullopt (Flutter null).
inline std::optional<BestMatchItem> readRecommendItem(
    const boost::json::value* v, std::string_view platform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        (v != nullptr && v->is_object()) ? v->get_object() : kEmpty;
    std::string type =
        json::firstNonEmpty(o, {"resourceType", "resource_type"});
    if (type.empty()) {
        return std::nullopt;
    }
    const auto* dataVal = o.if_contains(type);
    if (dataVal == nullptr || !dataVal->is_object() ||
        dataVal->get_object().empty()) {
        return std::nullopt;
    }
    BestMatchItem item;
    item.resourceType = type;
    if (type == "song") {
        item.data = parseSongInfo(*dataVal, platform);
    } else if (type == "artist") {
        item.data = parseArtistInfo(*dataVal, platform);
    } else if (type == "playlist") {
        item.data = parsePlaylistInfo(*dataVal, platform);
    } else if (type == "album") {
        item.data = parseAlbumInfo(*dataVal, platform);
    } else if (type == "mv") {
        item.data = parseMvInfo(*dataVal, platform);
    } else {
        return std::nullopt;
    }
    return item;
}

// `_readBestMatch`: primary first, then recommendations[]; nulls dropped.
inline std::vector<BestMatchItem> parseBestMatch(const boost::json::value* v,
                                                 std::string_view platform) {
    std::vector<BestMatchItem> out;
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        (v != nullptr && v->is_object()) ? v->get_object() : kEmpty;
    if (o.empty()) {
        return out;
    }
    if (auto primary = readRecommendItem(o.if_contains("primary"), platform)) {
        out.push_back(std::move(*primary));
    }
    if (const auto* recs = o.if_contains("recommendations");
        recs != nullptr && recs->is_array()) {
        for (const auto& entry : recs->get_array()) {
            if (auto item = readRecommendItem(&entry, platform)) {
                out.push_back(std::move(*item));
            }
        }
    }
    return out;
}

}  // namespace search_detail

// `/v1/search`. `keyword` echoes the body's `key` (when present, even empty),
// else the request keyword -- matching `'${data['key'] ?? keyword}'.trim()`.
inline ComprehensiveSearchResult parseComprehensiveSearch(
    const boost::json::value& body, std::string_view platform,
    std::string_view keyword) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    ComprehensiveSearchResult r;
    const auto* key = o.if_contains("key");
    r.keyword = (key != nullptr && !key->is_null()) ? json::toStr(*key)
                                                    : std::string(keyword);
    r.bestMatch = search_detail::parseBestMatch(
        model_detail::coalesce(o, "bestMatch", "best_match"), platform);
    r.song = search_detail::parseSection(o.if_contains("song"), platform,
                                         &parseSongInfo);
    r.playlist = search_detail::parseSection(o.if_contains("playlist"),
                                             platform, &parsePlaylistInfo);
    r.album = search_detail::parseSection(o.if_contains("album"), platform,
                                          &parseAlbumInfo);
    r.video = search_detail::parseSection(o.if_contains("mv"), platform,
                                          &parseMvInfo);
    r.artist = search_detail::parseSection(o.if_contains("artist"), platform,
                                           &parseArtistInfo);
    return r;
}

// `/v1/{type}/search` -> { list: [...] } typed for the given category. Every
// object entry kept (no id/name filter), matching searchMusic's raw list.
inline std::vector<SongInfo> parseSongSearch(const boost::json::value& body,
                                             std::string_view platform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;
    return discover_detail::mapAll(o.if_contains("list"), platform,
                                   &parseSongInfo);
}

inline std::vector<PlaylistInfo> parsePlaylistSearch(
    const boost::json::value& body, std::string_view platform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;
    return discover_detail::mapAll(o.if_contains("list"), platform,
                                   &parsePlaylistInfo);
}

inline std::vector<AlbumInfo> parseAlbumSearch(const boost::json::value& body,
                                               std::string_view platform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;
    return discover_detail::mapAll(o.if_contains("list"), platform,
                                   &parseAlbumInfo);
}

inline std::vector<ArtistInfo> parseArtistSearch(const boost::json::value& body,
                                                 std::string_view platform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;
    return discover_detail::mapAll(o.if_contains("list"), platform,
                                   &parseArtistInfo);
}

inline std::vector<MvInfo> parseVideoSearch(const boost::json::value& body,
                                            std::string_view platform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;
    return discover_detail::mapAll(o.if_contains("list"), platform,
                                   &parseMvInfo);
}

}  // namespace hemusic
