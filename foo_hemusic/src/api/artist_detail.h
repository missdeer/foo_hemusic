#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/album.h"
#include "api/artist.h"
#include "api/mv.h"
#include "api/song.h"
#include "net/json_codec.h"

// Artist detail endpoint parsers, ported from HE-Music-Flutter
// `features/artist/data/datasources/artist_detail_api_client.dart`. Four
// endpoints feed one page:
//   GET /v1/artist          -> meta (built into an ArtistInfo) + embedded songs
//   GET /v1/artist/songs    -> { list: [SongInfo],  has_more }
//   GET /v1/artist/albums   -> { list: [AlbumInfo], has_more }
//   GET /v1/artist/mvs      -> { list: [MvInfo],    has_more }
// Header-only; HTTP wiring lives in artist_detail_page.cpp.
//
// Like playlist_detail / album_detail, the meta uses different semantics from
// the shared parseArtistInfo:
//  - id / platform come from the request; the body name is the primary title,
//    falling back to `title`.
//  - alias is the only subtitle source (no fallback).
//  - description / cover follow the shared model_detail helpers.
//  - song / album / mv counts tolerate the same alias chain as parseArtistInfo
//    (mv_count ?? mvCount ?? video_count for mvs, *_count ?? *Count for the
//    rest), and clamp via countText so a "-1" never reaches the UI.

namespace hemusic {

struct ArtistDetailContent {
    ArtistInfo info;
    std::vector<SongInfo> songs;  // embedded under /v1/artist (page 1)
};

template <class T>
struct ArtistDetailPageChunk {
    std::vector<T> items;
    bool hasMore = false;
};

namespace artist_detail {

// `_resolveSongList` in the Flutter client: first array among the direct keys
// at top level, else the first such array inside a nested artist/data/detail
// object; nullptr if none. Mirrors album_detail::resolveSongList but the nested
// parent set is {artist, data, detail} (album_detail uses {data, detail,
// album}).
inline const boost::json::value* resolveSongList(const boost::json::object& o) {
    static constexpr const char* kDirect[] = {"songs", "tracks", "song_list",
                                              "songlist"};
    for (const char* k : kDirect) {
        const auto* v = o.if_contains(k);
        if (v != nullptr && v->is_array()) {
            return v;
        }
    }
    static constexpr const char* kNested[] = {"artist", "data", "detail"};
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

// `_readHasMore`: real bool passes through; otherwise "true"/"1".
inline bool readHasMore(const boost::json::object& o) {
    const auto* v = o.if_contains("has_more");
    if (v == nullptr) {
        return false;
    }
    return model_detail::boolean(v);
}

}  // namespace artist_detail

// `/v1/artist` -> ArtistInfo (meta) + embedded songs. `id`/`platform`/`title`
// come from the request; the body's name is the primary title, falling back to
// `title`.
inline ArtistDetailContent parseArtistDetailContent(
    const boost::json::value& body, std::string_view id,
    std::string_view platform, std::string_view title) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    ArtistDetailContent c;
    c.info.id = std::string(id);
    c.info.platform = std::string(platform);
    std::string name = json::str(o, "name");
    c.info.name = name.empty() ? std::string(title) : std::move(name);
    c.info.cover = model_detail::cover(o);
    c.info.description = json::str(o, "description");
    // mv_count alias chain matches parseArtistInfo (which handles mvCount /
    // video_count fallthrough on null entries).
    const boost::json::value* mv =
        model_detail::coalesce(o, "mv_count", "mvCount");
    if (mv == nullptr || mv->is_null()) {
        mv = o.if_contains("video_count");
    }
    c.info.mvCount = model_detail::countText(mv);
    c.info.songCount =
        model_detail::countTextCoalesce(o, "song_count", "songCount");
    c.info.albumCount =
        model_detail::countTextCoalesce(o, "album_count", "albumCount");
    c.info.alias = json::str(o, "alias");
    if (const auto* list = artist_detail::resolveSongList(o)) {
        c.songs = parseSongList(*list, platform);
    }
    return c;
}

// `/v1/artist/songs` -> { list: [SongInfo], has_more }. Reuses parseSongList,
// which drops songs with an empty id/name.
inline ArtistDetailPageChunk<SongInfo> parseArtistSongsPage(
    const boost::json::value& body, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    ArtistDetailPageChunk<SongInfo> chunk;
    if (const auto* list = o.if_contains("list")) {
        chunk.items = parseSongList(*list, fallbackPlatform);
    }
    chunk.hasMore = artist_detail::readHasMore(o);
    return chunk;
}

// `/v1/artist/albums` -> { list: [AlbumInfo], has_more }. Each item goes
// through parseAlbumInfo plus endpoint-specific alias overrides for
// `trackCount` / `createTime` (the Flutter artist client widens the count /
// publishTime alias sets beyond the shared parser). Entries with an empty
// id/name are dropped (matches the Flutter client's NetworkFailure throw at
// the boundary).
inline ArtistDetailPageChunk<AlbumInfo> parseArtistAlbumsPage(
    const boost::json::value& body, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    ArtistDetailPageChunk<AlbumInfo> chunk;
    if (const auto* list = o.if_contains("list");
        list != nullptr && list->is_array()) {
        for (const auto& item : list->get_array()) {
            AlbumInfo a = parseAlbumInfo(item, fallbackPlatform);
            if (a.id.empty() || a.name.empty()) {
                continue;
            }
            // Artist-endpoint widens the alias set vs the shared parser:
            // song_count gets a `trackCount` fallback, publish_time gets a
            // `createTime` fallback. Override only when the primary key chain
            // is fully absent in the source JSON -- inspecting the parser's
            // output (which clamps absent to "0") can't tell explicit-zero
            // from missing (Codex R2), so probe the input directly. id+name
            // non-empty guarantees the entry was an object, so get_object()
            // is safe here.
            const boost::json::object& io = item.get_object();
            if (model_detail::coalesce(io, "song_count", "songCount") ==
                nullptr) {
                if (const auto* tc = io.if_contains("trackCount");
                    tc != nullptr && !tc->is_null()) {
                    a.songCount = model_detail::countText(tc);
                }
            }
            if (model_detail::coalesce(io, "publish_time", "publishTime") ==
                nullptr) {
                if (const auto* ct = io.if_contains("createTime");
                    ct != nullptr && !ct->is_null()) {
                    a.publishTime = json::toStr(*ct);
                }
            }
            chunk.items.push_back(std::move(a));
        }
    }
    chunk.hasMore = artist_detail::readHasMore(o);
    return chunk;
}

// `/v1/artist/mvs` -> { list: [MvInfo], has_more }. parseMvInfo handles the
// shared aliases; this layer additionally widens play_count with a
// `watch_count` fallback (Flutter artist client).
inline ArtistDetailPageChunk<MvInfo> parseArtistMvsPage(
    const boost::json::value& body, std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    ArtistDetailPageChunk<MvInfo> chunk;
    if (const auto* list = o.if_contains("list");
        list != nullptr && list->is_array()) {
        for (const auto& item : list->get_array()) {
            MvInfo m = parseMvInfo(item, fallbackPlatform);
            if (m.id.empty() || m.name.empty()) {
                continue;
            }
            // Same 0-vs-absent care as the album path: probe the source
            // for the primary key chain instead of comparing the parser's
            // clamped output (Codex R2). id+name non-empty guarantees the
            // entry was an object.
            const boost::json::object& io = item.get_object();
            if (model_detail::coalesce(io, "play_count", "playCount") ==
                nullptr) {
                if (const auto* wc = io.if_contains("watch_count");
                    wc != nullptr && !wc->is_null()) {
                    m.playCount = model_detail::countText(wc);
                }
            }
            chunk.items.push_back(std::move(m));
        }
    }
    chunk.hasMore = artist_detail::readHasMore(o);
    return chunk;
}

}  // namespace hemusic
