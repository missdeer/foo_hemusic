#pragma once

#include <string>
#include <string_view>

#include <boost/json.hpp>

#include "api/song.h"
#include "net/json_codec.h"

// ArtistInfo domain model + parser, ported from HE-Music-Flutter
// `shared/models/he_music_models.dart` (ArtistInfo). Used by search artist
// results and the artist detail page (header + tabs). Reuses the shared
// model_detail helpers from api/song.h. Header-only like the rest of api/.
//
// Matching album.h / playlist.h, only the single-item parser is provided: the
// Flutter source has no top-level ArtistInfo list helper with a filter, so list
// mapping is left to the endpoint layer (search) when it is built.

namespace hemusic {

struct ArtistInfo {
    std::string id;
    std::string name;
    std::string cover;
    std::string platform;
    std::string description;
    std::string mvCount;     // text; "0" when absent/negative
    std::string songCount;   // text
    std::string albumCount;  // text
    std::string alias;
};

// `ArtistInfo.fromMap`. mv_count tolerates the `mvCount` / `video_count`
// aliases; song_count / album_count tolerate their camelCase aliases.
inline ArtistInfo parseArtistInfo(const boost::json::value& value,
                                  std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    ArtistInfo a;
    a.id = json::str(o, "id");
    a.name = json::str(o, "name");
    a.cover = model_detail::cover(o);
    std::string platform = json::str(o, "platform");
    a.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    a.description = json::str(o, "description");
    // `mv_count ?? mvCount ?? video_count`: first present-non-null wins.
    const boost::json::value* mv =
        model_detail::coalesce(o, "mv_count", "mvCount");
    if (mv == nullptr || mv->is_null()) {
        mv = o.if_contains("video_count");
    }
    a.mvCount = model_detail::countText(mv);
    a.songCount = model_detail::countTextCoalesce(o, "song_count", "songCount");
    a.albumCount =
        model_detail::countTextCoalesce(o, "album_count", "albumCount");
    a.alias = json::str(o, "alias");
    return a;
}

}  // namespace hemusic
