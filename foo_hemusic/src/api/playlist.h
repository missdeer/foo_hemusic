#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/song.h"
#include "net/json_codec.h"

// PlaylistInfo (+ its CategoryInfo) domain model + parsers, ported from
// HE-Music-Flutter `shared/models/he_music_models.dart` (PlaylistInfo /
// CategoryInfo). Used by the discover featured-playlists section, the playlist
// square, playlist detail, and the user's self-built playlists. Reuses the
// shared model_detail helpers from api/song.h. Header-only like the rest of
// api/.

namespace hemusic {

struct CategoryInfo {
    std::string name;
    std::string id;
    std::string platform;
};

// `_categories`: array of objects/scalars -> CategoryInfo list (empty names
// dropped). Note Flutter calls CategoryInfo.fromMap without a fallback
// platform, so a category's platform is only what its own payload carries.
inline std::vector<CategoryInfo> parseCategoryList(
    const boost::json::value* v) {
    std::vector<CategoryInfo> out;
    if (v == nullptr || !v->is_array()) {
        return out;
    }
    for (const auto& item : v->get_array()) {
        CategoryInfo c;
        if (item.is_object()) {
            const auto& o = item.get_object();
            c = CategoryInfo{json::str(o, "name"), json::str(o, "id"),
                             json::str(o, "platform")};
        } else {
            c = CategoryInfo{json::toStr(item), "", ""};
        }
        if (!c.name.empty()) {
            out.push_back(std::move(c));
        }
    }
    return out;
}

struct PlaylistInfo {
    std::string name;
    std::string id;
    std::string cover;
    std::string creator;
    std::string songCount;  // text; "0" when absent/negative
    std::string playCount;  // text
    std::vector<SongInfo> songs;
    std::string platform;
    std::string description;
    std::vector<CategoryInfo> categories;
    bool isDefault = false;
};

// `PlaylistInfo.fromMap`. song_count/play_count tolerate camelCase aliases;
// is_default is true on either the int 1 or a truthy bool/string.
inline PlaylistInfo parsePlaylistInfo(const boost::json::value& value,
                                      std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    PlaylistInfo p;
    p.name = json::str(o, "name");
    p.id = json::str(o, "id");
    p.cover = model_detail::cover(o);
    p.creator = json::str(o, "creator");
    p.songCount = model_detail::countTextCoalesce(o, "song_count", "songCount");
    p.playCount = model_detail::countTextCoalesce(o, "play_count", "playCount");
    if (const auto* songs = o.if_contains("songs")) {
        p.songs = parseSongList(*songs, fallbackPlatform);
    }
    std::string platform = json::str(o, "platform");
    p.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    p.description = json::str(o, "description");
    p.categories = parseCategoryList(o.if_contains("categories"));
    const auto* def = o.if_contains("is_default");
    p.isDefault = (def != nullptr && json::toI64(*def) == 1) ||
                  model_detail::boolean(def);
    return p;
}

}  // namespace hemusic
