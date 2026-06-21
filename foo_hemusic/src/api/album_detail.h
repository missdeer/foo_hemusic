#pragma once

#include <string>
#include <string_view>

#include <boost/json.hpp>

#include "api/album.h"
#include "api/song.h"
#include "net/json_codec.h"

// Album detail endpoint parser, ported from HE-Music-Flutter
// `features/album/data/datasources/album_detail_api_client.dart`. A single
// endpoint GET /v1/album returns both the album meta and its songs (embedded),
// so one parser yields a full AlbumInfo with songs. Header-only; HTTP wiring
// added with the Phase 5 album page.
//
// Detail-specific divergences from the shared `parseAlbumInfo`, faithfully
// mirroring the datasource's private helpers:
//  - songs are located via `_resolveSongList` (songs/tracks/song_list/songlist,
//    also under a nested data/detail/album object), not just "songs".
//  - artists read ONLY the `artists` key (no `artist` alias here).
//  - song_count gains a `trackCount` alias and falls back to the song count.
//  - publish_time gains a `createTime` alias.
//  - play_count is the raw trimmed string (not the "0"-clamped countText).
//  - id/name/platform come from the request (body name is the primary title).
// Songs go through parseSongList, which drops entries with an empty id/name
// (unplayable). Flutter `_songs` throws on those; the lenient parser drops
// them, matching the player-safety stance in playlist_detail.h / ranking.h.

namespace hemusic {

namespace album_detail {

// `_resolveSongList`: first array among the direct keys at top level, else the
// first such array inside a nested data/detail/album object; nullptr if none.
inline const boost::json::value* resolveSongList(const boost::json::object& o) {
    static constexpr const char* kDirect[] = {"songs", "tracks", "song_list",
                                              "songlist"};
    for (const char* k : kDirect) {
        const auto* v = o.if_contains(k);
        if (v != nullptr && v->is_array()) {
            return v;
        }
    }
    static constexpr const char* kNested[] = {"data", "detail", "album"};
    for (const char* pk : kNested) {
        const auto* parent = o.if_contains(pk);
        if (parent == nullptr || !parent->is_object()) {
            continue;
        }
        const auto& po = parent->get_object();
        for (const char* k : kDirect) {
            const auto* v = po.if_contains(k);
            if (v != nullptr && v->is_array()) {
                return v;
            }
        }
    }
    return nullptr;
}

}  // namespace album_detail

// `/v1/album` -> AlbumInfo (with embedded songs). `id`/`platform`/`title` come
// from the request; the body's name is the primary title, falling back to
// `title`.
inline AlbumInfo parseAlbumDetailInfo(const boost::json::value& body,
                                      std::string_view id,
                                      std::string_view platform,
                                      std::string_view title) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    AlbumInfo a;
    std::string name = json::str(o, "name");
    a.name = name.empty() ? std::string(title) : std::move(name);
    a.id = std::string(id);
    a.cover = model_detail::cover(o);
    // Detail reads only the `artists` key (no `artist` alias).
    a.artists = model_detail::artists(o.if_contains("artists"));
    if (const auto* list = album_detail::resolveSongList(o)) {
        a.songs = parseSongList(*list, platform);
    }
    a.songCount =
        json::firstNonEmpty(o, {"song_count", "songCount", "trackCount"});
    if (a.songCount.empty()) {
        a.songCount = std::to_string(a.songs.size());
    }
    a.publishTime =
        json::firstNonEmpty(o, {"publish_time", "publishTime", "createTime"});
    a.description = json::str(o, "description");
    a.platform = std::string(platform);
    a.language = json::str(o, "language");
    a.genre = json::str(o, "genre");
    a.type = json::i64(o, "type");
    a.isFinished = model_detail::boolean(o.if_contains("is_finished"));
    a.playCount = json::str(o, "play_count");  // raw, not countText
    return a;
}

}  // namespace hemusic
