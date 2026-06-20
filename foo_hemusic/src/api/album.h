#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/song.h"
#include "net/json_codec.h"

// AlbumInfo domain model + parser, ported from HE-Music-Flutter
// `shared/models/he_music_models.dart` (AlbumInfo). Used by the discover
// new-albums section, search album results, the artist albums tab, and album
// detail. Reuses the shared model_detail helpers from api/song.h. Header-only
// like the rest of
// api/.

namespace hemusic {

struct AlbumInfo {
    std::string name;
    std::string id;
    std::string cover;
    std::vector<SongArtist> artists;
    std::string songCount;    // text; "0" when absent/negative
    std::string publishTime;  // publish_time ?? publishTime
    std::vector<SongInfo> songs;
    std::string description;
    std::string platform;
    std::string language;
    std::string genre;
    long long type = 0;
    bool isFinished = false;
    std::string playCount;  // text
};

// `AlbumInfo.fromMap`. artists tolerate the `artist` alias; song_count /
// publish_time / play_count tolerate their camelCase aliases.
inline AlbumInfo parseAlbumInfo(const boost::json::value& value,
                                std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    AlbumInfo a;
    a.name = json::str(o, "name");
    a.id = json::str(o, "id");
    a.cover = model_detail::cover(o);
    a.artists =
        model_detail::artists(model_detail::coalesce(o, "artists", "artist"));
    a.songCount = model_detail::countTextCoalesce(o, "song_count", "songCount");
    a.publishTime = model_detail::strCoalesce(o, "publish_time", "publishTime");
    if (const auto* songs = o.if_contains("songs")) {
        a.songs = parseSongList(*songs, fallbackPlatform);
    }
    a.description = json::str(o, "description");
    std::string platform = json::str(o, "platform");
    a.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    a.language = json::str(o, "language");
    a.genre = json::str(o, "genre");
    a.type = json::i64(o, "type");
    a.isFinished = model_detail::boolean(o.if_contains("is_finished"));
    a.playCount = model_detail::countTextCoalesce(o, "play_count", "playCount");
    return a;
}

}  // namespace hemusic
