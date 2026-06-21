#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/playlist.h"
#include "api/song.h"
#include "net/json_codec.h"

// Playlist detail endpoint parsers, ported from HE-Music-Flutter
// `features/playlist/data/datasources/playlist_detail_api_client.dart`. Two
// endpoints feed one page:
//   GET /v1/playlist        -> meta (built into a PlaylistInfo)
//   GET /v1/playlist/songs  -> { list: [SongInfo] }
// The Flutter client issues both and stitches the songs into
// PlaylistInfo.songs; here the two responses are parsed separately and the
// endpoint/UI layer combines them. Header-only; HTTP wiring added with the
// Phase 5 playlist page.
//
// NOTE the detail meta uses DIFFERENT count semantics from the shared
// `parsePlaylistInfo`: song_count/play_count are kept as the raw trimmed string
// (empty when absent, NOT clamped to "0"), song_count gains a `trackCount`
// alias, an empty creator becomes "-", and id/name/platform come from the
// request (the body's name is only a fallback source). This faithfully mirrors
// the datasource's private `_title`/`_subtitle`/`_songCount`/`_playCount`.

namespace hemusic {

// `/v1/playlist` meta -> PlaylistInfo (songs left empty; fill from
// parsePlaylistSongs). `id`/`platform`/`title` come from the request: the
// body's name is only consulted as the primary title, falling back to `title`.
inline PlaylistInfo parsePlaylistDetailInfo(const boost::json::value& body,
                                            std::string_view id,
                                            std::string_view platform,
                                            std::string_view title) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    PlaylistInfo p;
    std::string name = json::str(o, "name");
    p.name = name.empty() ? std::string(title) : std::move(name);
    p.id = std::string(id);
    p.cover = model_detail::cover(o);
    std::string creator = json::str(o, "creator");
    p.creator = creator.empty() ? "-" : std::move(creator);
    p.songCount =
        json::firstNonEmpty(o, {"song_count", "songCount", "trackCount"});
    p.playCount = json::firstNonEmpty(o, {"play_count", "playCount"});
    p.platform = std::string(platform);
    p.description = json::str(o, "description");
    return p;
}

// `/v1/playlist/songs` -> { list: [...] }. Reuses parseSongList, which drops
// songs with an empty id/name (Flutter throws on those; the lenient parser
// drops, matching our player-safety stance).
inline std::vector<SongInfo> parsePlaylistSongs(
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
