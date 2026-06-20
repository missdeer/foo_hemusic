#pragma once

#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/album.h"
#include "api/mv.h"
#include "api/playlist.h"
#include "api/song.h"
#include "net/json_codec.h"

// GET /v1/page/discover response model + parser, ported from HE-Music-Flutter
// `features/home/data/datasources/home_discover_api_client.dart`
// (fetchDiscoverSections). The page is four parallel sections keyed
// new_songs / new_albums / featured_playlists / featured_mvs, each parsed with
// the selected platform as the fallback for its items. Header-only; the HTTP
// call (GET with ?platform=, via ApiClient) is wired up with the Phase 4 UI
// that owns platform selection.
//
// Contract (mirrors the Flutter datasource's `_parseList`, applied uniformly to
// all four sections):
//  - No id/name filtering at the section level -- every object entry is kept,
//    just like `_parseList(... .fromMap)`. (The id/name `where` filter only
//    exists inside `_songs`, i.e. the songs nested within a Playlist/Album, not
//    the top-level new_songs section -- so new_songs is NOT filtered either.)
//  - Deliberate lenient divergence: Flutter `_parseList` THROWS on a missing /
//    non-array section and `_asMap` THROWS on a non-object item. A pure parser
//    must not throw, so a missing-or-non-array section degrades to an empty
//    vector and a non-object item is skipped. A browser renders an
//    empty/partial page more gracefully than it crashes, and the caller can
//    still detect a wholly-empty page.

namespace hemusic {

struct DiscoverPage {
    std::vector<SongInfo> newSongs;
    std::vector<AlbumInfo> newAlbums;
    std::vector<PlaylistInfo> featuredPlaylists;
    std::vector<MvInfo> featuredMvs;
};

namespace discover_detail {

// Maps each array element through `parse`, mirroring the datasource
// `_parseList`: no id/name filtering (unlike model-internal `_songs`).
// Non-array / absent -> empty. Non-object items are skipped rather than turned
// into empty shells -- Flutter's `_asMap` throws on a non-map item, so the
// faithful lenient equivalent is to drop it, not keep a blank entry.
template <typename T>
std::vector<T> mapAll(const boost::json::value* v, std::string_view fallback,
                      T (*parse)(const boost::json::value&, std::string_view)) {
    std::vector<T> out;
    if (v == nullptr || !v->is_array()) {
        return out;
    }
    const auto& arr = v->get_array();
    out.reserve(arr.size());
    for (const auto& item : arr) {
        if (!item.is_object()) {
            continue;
        }
        out.push_back(parse(item, fallback));
    }
    return out;
}

}  // namespace discover_detail

inline DiscoverPage parseDiscoverPage(const boost::json::value& body,
                                      std::string_view platformId) {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        body.is_object() ? body.get_object() : kEmpty;

    DiscoverPage page;
    page.newSongs = discover_detail::mapAll(o.if_contains("new_songs"),
                                            platformId, &parseSongInfo);
    page.newAlbums = discover_detail::mapAll(o.if_contains("new_albums"),
                                             platformId, &parseAlbumInfo);
    page.featuredPlaylists = discover_detail::mapAll(
        o.if_contains("featured_playlists"), platformId, &parsePlaylistInfo);
    page.featuredMvs = discover_detail::mapAll(o.if_contains("featured_mvs"),
                                               platformId, &parseMvInfo);
    return page;
}

}  // namespace hemusic
