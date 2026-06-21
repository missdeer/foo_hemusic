#pragma once

#include <cctype>
#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/song.h"
#include "net/json_codec.h"

// Ranking endpoint models + parsers, ported from HE-Music-Flutter
// `features/ranking/data/datasources/ranking_api_client.dart`. Two endpoints:
//   GET /v1/rankings  -> { groups: [RankingGroup] }   (browse)
//   GET /v1/ranking   -> RankingDetail                 (one chart's songs)
// Header-only; HTTP wiring added with the Phase 5 ranking page.
//
// Lenient divergences (consistent with discover.h / platforms.h): Flutter
// throws on a missing/non-array `groups` and on non-map items; the pure parser
// degrades to empty and skips non-object items. RankingDetail.songs reuses
// parseSongList, dropping unplayable songs (Flutter `_parseSongs` throws).

namespace hemusic {

struct RankingPreviewSong {
    std::string name;
    std::string artist;
};

struct RankingInfo {
    std::string id;
    std::string platform;
    std::string name;
    std::string coverUrl;
    std::vector<RankingPreviewSong> previewSongs;  // first 3 songs
};

struct RankingGroup {
    std::string name;
    std::vector<RankingInfo> rankings;
};

struct RankingDetail {
    RankingInfo info;
    std::vector<SongInfo> songs;
    bool hasMore = false;
    std::string lastId;
    long long totalCount = 0;
    std::string description;
};

namespace ranking_detail {

// `_readBool`: first key that is a bool / non-zero number / "true"|"1" (true)
// or "false"|"0" (false); other values are skipped; default false.
inline bool readBoolKeys(const boost::json::object& o,
                         std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const auto* v = o.if_contains(k);
        if (v == nullptr) {
            continue;
        }
        if (v->is_bool()) {
            return v->get_bool();
        }
        if (v->is_int64() || v->is_uint64() || v->is_double()) {
            return json::toI64(*v) != 0;
        }
        std::string s = json::toStr(*v);
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (s == "true" || s == "1") {
            return true;
        }
        if (s == "false" || s == "0") {
            return false;
        }
    }
    return false;
}

// `_readInt`: first key that is an int or a fully-parseable numeric string;
// doubles and other values are skipped (matching Dart `int.tryParse('$v')`);
// default 0.
inline long long readIntKeys(const boost::json::object& o,
                             std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const auto* v = o.if_contains(k);
        if (v == nullptr) {
            continue;
        }
        if (v->is_int64()) {
            return v->get_int64();
        }
        if (v->is_uint64()) {
            return static_cast<long long>(v->get_uint64());
        }
        if (v->is_string()) {
            const auto& s = v->get_string();
            long long out = 0;
            const char* begin = s.c_str();
            const char* end = begin + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, out);
            if (ec == std::errc{} && ptr == end) {
                return out;
            }
        }
        // is_double / non-numeric string: skip to the next key.
    }
    return 0;
}

}  // namespace ranking_detail

// `_parseRankingInfo`. id falls back to `fallbackId` then "-"; platform to
// `fallbackPlatform`; name (name/title) to "-"; cover from cover/pic/imgurl/
// image (note: no "thumb" here). previewSongs are the first 3 songs' name +
// joined artist, each "-" when empty.
inline RankingInfo parseRankingInfo(const boost::json::value& value,
                                    std::string_view fallbackPlatform,
                                    std::string_view fallbackId = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    RankingInfo r;
    std::string id = json::firstNonEmpty(o, {"id"});
    r.id = id.empty() ? (fallbackId.empty() ? std::string("-")
                                            : std::string(fallbackId))
                      : std::move(id);
    std::string platform = json::firstNonEmpty(o, {"platform"});
    r.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    std::string name = json::firstNonEmpty(o, {"name", "title"});
    r.name = name.empty() ? std::string("-") : std::move(name);
    r.coverUrl = json::firstNonEmpty(o, {"cover", "pic", "imgurl", "image"});

    const auto* songs = o.if_contains("songs");
    if (songs != nullptr && songs->is_array()) {
        int taken = 0;
        for (const auto& item : songs->get_array()) {
            if (taken >= 3) {
                break;
            }
            if (!item.is_object()) {
                continue;  // skip non-object entries without consuming a slot
            }
            ++taken;
            const boost::json::object& so = item.get_object();
            std::string songName = json::firstNonEmpty(so, {"name", "title"});
            SongInfo s = parseSongInfo(item, fallbackPlatform);
            r.previewSongs.push_back(
                {songName.empty() ? std::string("-") : std::move(songName),
                 songArtistText(s)});
        }
    }
    return r;
}

// `_parseGroup`. Group name (name/title) falls back to "榜单" (UTF-8 bytes, so
// the header compiles regardless of the source code page).
inline RankingGroup parseRankingGroup(const boost::json::value& value,
                                      std::string_view fallbackPlatform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    RankingGroup g;
    std::string name = json::firstNonEmpty(o, {"name", "title"});
    g.name = name.empty() ? std::string("\xE6\xA6\x9C\xE5\x8D\x95")  // 榜单
                          : std::move(name);
    const auto* rankings = o.if_contains("rankings");
    if (rankings != nullptr && rankings->is_array()) {
        for (const auto& item : rankings->get_array()) {
            if (!item.is_object()) {
                continue;
            }
            g.rankings.push_back(parseRankingInfo(item, fallbackPlatform));
        }
    }
    return g;
}

// `/v1/rankings` -> { groups: [...] }. Missing/non-array groups -> empty.
inline std::vector<RankingGroup> parseRankingGroups(
    const boost::json::value& body, std::string_view fallbackPlatform) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    std::vector<RankingGroup> out;
    const auto* groups = o.if_contains("groups");
    if (groups == nullptr || !groups->is_array()) {
        return out;
    }
    for (const auto& item : groups->get_array()) {
        if (!item.is_object()) {
            continue;
        }
        out.push_back(parseRankingGroup(item, fallbackPlatform));
    }
    return out;
}

// `/v1/ranking` -> RankingDetail. The chart meta is parsed from the same
// top-level payload (with `fallbackId` = the requested id), songs from the
// `songs` array.
inline RankingDetail parseRankingDetail(const boost::json::value& body,
                                        std::string_view fallbackPlatform,
                                        std::string_view fallbackId) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    RankingDetail d;
    d.info = parseRankingInfo(body, fallbackPlatform, fallbackId);
    if (const auto* songs = o.if_contains("songs")) {
        d.songs = parseSongList(*songs, fallbackPlatform);
    }
    d.hasMore = ranking_detail::readBoolKeys(o, {"has_more", "hasMore"});
    d.lastId = json::firstNonEmpty(o, {"last_id", "lastId"});
    d.totalCount =
        ranking_detail::readIntKeys(o, {"total_count", "totalCount"});
    d.description = json::firstNonEmpty(o, {"description", "desc"});
    return d;
}

}  // namespace hemusic
