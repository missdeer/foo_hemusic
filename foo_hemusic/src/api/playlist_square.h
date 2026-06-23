#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/playlist.h"
#include "net/json_codec.h"

// Playlist square (歌单广场) endpoints, ported from HE-Music-Flutter
// `features/playlist/data/datasources/playlist_plaza_api_client.dart`. Two
// endpoints:
//   GET /v1/playlist/categories?platform=
//       -> { groups: [{ name, categories: [CategoryInfo] }] }
//   GET /v1/category/playlists?platform=&category_id=&page_index=&page_size=
//       -> { list: [PlaylistInfo], has_more, last_id }
//
// Header-only; HTTP wiring (via ApiClient) lives in the square page.
//
// Lenient divergence (consistent with discover.h / radio.h): Flutter throws on
// missing/non-array `groups` or `list`; the pure parser degrades to empty.

namespace hemusic {

struct PlaylistCategoryGroup {
    std::string name;
    std::vector<CategoryInfo> categories;
};

struct PlaylistSquarePage {
    std::vector<PlaylistInfo> list;
    bool hasMore = false;
    std::string lastId;
};

// Default page size for /v1/category/playlists, matching the Flutter client's
// `pageSize = 30` default (which is also what the backend hands out when the
// caller passes no override).
inline constexpr int kPlaylistSquarePageSize = 30;

namespace playlist_square_detail {

// Mirrors radio/ranking _readBool: bool / non-zero number / "true"|"1" -> true;
// "false"|"0" -> false; otherwise `fallback`.
inline bool readBoolField(const boost::json::object& o, const char* key,
                          bool fallback) {
    const auto* v = o.if_contains(key);
    if (v == nullptr) {
        return fallback;
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
    return fallback;
}

}  // namespace playlist_square_detail

// `PlaylistCategoryGroup.fromMap`. Drop categories whose name is empty
// (matches Flutter's `.where((item) => item.name.trim().isNotEmpty)`); accept
// scalar category items (treated as bare names) the same way Flutter does.
inline PlaylistCategoryGroup parsePlaylistCategoryGroup(
    const boost::json::value& value, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    PlaylistCategoryGroup g;
    g.name = json::str(o, "name");
    const auto* raw = o.if_contains("categories");
    if (raw == nullptr || !raw->is_array()) {
        return g;
    }
    for (const auto& item : raw->get_array()) {
        CategoryInfo c;
        if (item.is_object()) {
            const auto& co = item.get_object();
            std::string platform = json::str(co, "platform");
            c = CategoryInfo{json::str(co, "name"), json::str(co, "id"),
                             platform.empty() ? std::string(fallbackPlatform)
                                              : std::move(platform)};
        } else {
            // json::toStr already trims (see json_codec.h::toStr), matching
            // Flutter's `'$value'.trim()` semantics.
            c = CategoryInfo{json::toStr(item), "",
                             std::string(fallbackPlatform)};
        }
        if (!c.name.empty()) {
            g.categories.push_back(std::move(c));
        }
    }
    return g;
}

// `/v1/playlist/categories` -> { groups: [...] }. Missing/non-array groups ->
// empty (lenient: Flutter throws).
inline std::vector<PlaylistCategoryGroup> parsePlaylistCategoryGroups(
    const boost::json::value& body, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    std::vector<PlaylistCategoryGroup> out;
    const auto* groups = o.if_contains("groups");
    if (groups == nullptr || !groups->is_array()) {
        return out;
    }
    for (const auto& item : groups->get_array()) {
        if (!item.is_object()) {
            continue;  // lenient: Flutter _asMap throws
        }
        out.push_back(parsePlaylistCategoryGroup(item, fallbackPlatform));
    }
    return out;
}

// `/v1/category/playlists` -> { list, has_more, last_id }. has_more falls back
// to (list.size >= page_size) when the field is missing -- mirrors Flutter's
// `fallback: list.length >= safePageSize`. Caller passes the request's page
// size; pass 0 to disable the fallback (rare).
inline PlaylistSquarePage parseCategoryPlaylistsPage(
    const boost::json::value& body, std::string_view fallbackPlatform = "",
    int pageSize = kPlaylistSquarePageSize) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    PlaylistSquarePage p;
    const auto* list = o.if_contains("list");
    if (list != nullptr && list->is_array()) {
        for (const auto& item : list->get_array()) {
            if (!item.is_object()) {
                continue;
            }
            p.list.push_back(parsePlaylistInfo(item, fallbackPlatform));
        }
    }
    const bool sizeFallback =
        pageSize > 0 && p.list.size() >= static_cast<std::size_t>(pageSize);
    p.hasMore =
        playlist_square_detail::readBoolField(o, "has_more", sizeFallback);
    p.lastId = json::firstNonEmpty(o, {"last_id", "lastId"});
    return p;
}

}  // namespace hemusic
