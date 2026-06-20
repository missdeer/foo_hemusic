#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "api/song.h"
#include "net/json_codec.h"

// MvInfo domain model + parser, ported from HE-Music-Flutter
// `shared/models/he_music_models.dart` (MvInfo). Used by the discover
// featured-mvs section, artist MV tab, and (Phase 2 nice-to-have) MV playback.
// Reuses the shared model_detail helpers from api/song.h. Header-only.

namespace hemusic {

struct MvInfo {
    std::string platform;
    std::vector<SongLink> links;
    std::string id;
    std::string name;
    std::string cover;
    long long type = 0;
    std::string playCount;  // text; "0" when absent/negative
    std::string creator;
    long long duration = 0;
    std::string description;
};

// `MvInfo.fromMap`. play_count tolerates the camelCase alias; platform falls
// back to fallbackPlatform when the payload omits it.
inline MvInfo parseMvInfo(const boost::json::value& value,
                          std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    MvInfo m;
    std::string platform = json::str(o, "platform");
    m.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    m.links = model_detail::links(o.if_contains("links"));
    m.id = json::str(o, "id");
    m.name = json::str(o, "name");
    m.cover = model_detail::cover(o);
    m.type = json::i64(o, "type");
    m.playCount = model_detail::countTextCoalesce(o, "play_count", "playCount");
    m.creator = json::str(o, "creator");
    m.duration = json::i64(o, "duration");
    m.description = json::str(o, "description");
    return m;
}

}  // namespace hemusic
