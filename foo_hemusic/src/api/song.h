#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "net/json_codec.h"

// Shared SongInfo domain model + parsers, ported from HE-Music-Flutter
// `shared/models/he_music_models.dart` (SongInfo / LinkInfo /
// SongInfoArtistInfo / SongInfoAlbumInfo and their `_*` helpers). This is the
// most reused list primitive -- discover / search / playlist / album responses
// all carry it, and the playback layer derives `hemusic://` hint params from it
// -- so it lives in the api/ layer header-only like user.h and is unit-tested
// in isolation.
//
// Field-alias / type tolerance mirrors the Flutter `fromMap` exactly:
//  - `name` falls back to `title`, `artists` to `artist`, via Dart `??`
//    (null/absent only -- a present empty string does NOT fall back).
//  - cover is the first non-empty of cover/pic/imgurl/image/thumb.
//  - list parsers drop entries failing the same `where` filters Flutter uses.

namespace hemusic {

struct SongArtist {
    std::string id;
    std::string name;
};

struct SongAlbum {
    std::string name;
    std::string id;
};

struct SongLink {
    std::string name;
    long long quality = 0;
    std::string format;
    std::string size;
    std::string url;
};

struct SongInfo {
    std::string name;
    std::string subtitle;
    std::string id;
    long long duration = 0;
    std::string mvId;
    std::optional<SongAlbum> album;
    std::vector<SongArtist> artists;
    std::vector<SongLink> links;
    std::string platform;
    std::string cover;
    std::vector<SongInfo> sublist;
    long long originalType = 0;
    std::optional<std::string> path;
    std::optional<long long> size;
    std::optional<std::string> quality;
    std::optional<std::string> alias;
};

// --- leaf helpers (faithful ports of the dart `_*` free functions) -----------
// Shared across the api/ model headers (song / playlist / album / mv), not just
// SongInfo, so the helpers live here as the first model header and are reused
// by the others.
namespace model_detail {

// `_countText`: non-negative integer as text; null/negative/unparseable -> "0".
inline std::string countText(const boost::json::value* v) {
    if (v == nullptr) {
        return "0";
    }
    std::string text = json::toStr(*v);
    if (text.empty()) {
        return "0";
    }
    long long n = json::toI64(*v);
    return n < 0 ? "0" : std::to_string(n);
}

// `_countText` for `raw[k1] ?? raw[k2]` (e.g. song_count / songCount aliases).
inline std::string countTextCoalesce(const boost::json::object& o,
                                     std::string_view k1, std::string_view k2);

// `_bool`: real bool passes through; otherwise "true"/"1" (trimmed,
// lowercased).
inline bool boolean(const boost::json::value* v) {
    if (v == nullptr) {
        return false;
    }
    if (v->is_bool()) {
        return v->get_bool();
    }
    std::string s = json::toStr(*v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "true" || s == "1";
}

// Dart `raw[k1] ?? raw[k2]`: yields k1 only when present and non-null,
// otherwise k2 (which may itself be absent -> nullptr). NOT triggered by a
// present empty string, matching `??`.
inline const boost::json::value* coalesce(const boost::json::object& o,
                                          std::string_view k1,
                                          std::string_view k2) {
    const auto* p = o.if_contains(k1);
    if (p != nullptr && !p->is_null()) {
        return p;
    }
    return o.if_contains(k2);
}

inline std::string countTextCoalesce(const boost::json::object& o,
                                     std::string_view k1, std::string_view k2) {
    return countText(coalesce(o, k1, k2));
}

// `_string(raw[k1] ?? raw[k2])`.
inline std::string strCoalesce(const boost::json::object& o,
                               std::string_view k1, std::string_view k2) {
    const auto* p = o.if_contains(k1);
    if (p != nullptr && !p->is_null()) {
        return json::toStr(*p);
    }
    const auto* q = o.if_contains(k2);
    return q != nullptr ? json::toStr(*q) : std::string{};
}

// `_cover`: first non-empty of cover/pic/imgurl/image/thumb.
inline std::string cover(const boost::json::object& o) {
    for (const char* key : {"cover", "pic", "imgurl", "image", "thumb"}) {
        std::string v = json::str(o, key);
        if (!v.empty()) {
            return v;
        }
    }
    return {};
}

// `_nullableString`: empty/absent -> nullopt.
inline std::optional<std::string> nullableStr(const boost::json::value* v) {
    if (v == nullptr) {
        return std::nullopt;
    }
    std::string s = json::toStr(*v);
    if (s.empty()) {
        return std::nullopt;
    }
    return s;
}

// `_nullableInt`: null/absent -> nullopt; a present value that coerces to 0 but
// stringifies non-empty (e.g. "abc") still returns 0, only empty text -> null.
inline std::optional<long long> nullableInt(const boost::json::value* v) {
    if (v == nullptr || v->is_null()) {
        return std::nullopt;
    }
    long long result = json::toI64(*v);
    if (result == 0 && json::toStr(*v).empty()) {
        return std::nullopt;
    }
    return result;
}

// `_album`: object -> {name,id}; non-empty scalar -> {text,""}; else nullopt.
inline std::optional<SongAlbum> album(const boost::json::value* v) {
    if (v == nullptr) {
        return std::nullopt;
    }
    if (v->is_object()) {
        const auto& a = v->get_object();
        return SongAlbum{json::str(a, "name"), json::str(a, "id")};
    }
    std::string text = json::toStr(*v);
    if (text.empty()) {
        return std::nullopt;
    }
    return SongAlbum{text, ""};
}

// `_artists`: list of objects/scalars -> list (empty names dropped); a single
// non-empty scalar -> one unnamed-id artist.
inline std::vector<SongArtist> artists(const boost::json::value* v) {
    std::vector<SongArtist> out;
    if (v == nullptr) {
        return out;
    }
    if (v->is_array()) {
        for (const auto& item : v->get_array()) {
            SongArtist a;
            if (item.is_object()) {
                const auto& o = item.get_object();
                a = SongArtist{json::str(o, "id"), json::str(o, "name")};
            } else {
                a = SongArtist{"", json::toStr(item)};
            }
            if (!a.name.empty()) {
                out.push_back(std::move(a));
            }
        }
        return out;
    }
    std::string text = json::toStr(*v);
    if (!text.empty()) {
        out.push_back(SongArtist{"", text});
    }
    return out;
}

// `_links`: list only; entries kept when quality>0 OR url non-empty.
inline std::vector<SongLink> links(const boost::json::value* v) {
    std::vector<SongLink> out;
    if (v == nullptr || !v->is_array()) {
        return out;
    }
    for (const auto& item : v->get_array()) {
        SongLink l;
        if (item.is_object()) {
            const auto& o = item.get_object();
            l = SongLink{json::str(o, "name"), json::i64(o, "quality"),
                         json::str(o, "format"), json::str(o, "size"),
                         json::str(o, "url")};
        }
        if (l.quality > 0 || !l.url.empty()) {
            out.push_back(std::move(l));
        }
    }
    return out;
}

}  // namespace model_detail

// Forward declaration for the mutual recursion through `sublist`.
inline std::vector<SongInfo> parseSongList(
    const boost::json::value& value, std::string_view fallbackPlatform = "");

// `SongInfo.fromMap`. A non-object yields an all-empty SongInfo (id/name
// empty), which `parseSongList` then filters out -- matching Flutter `_songs`.
inline SongInfo parseSongInfo(const boost::json::value& value,
                              std::string_view fallbackPlatform = "") {
    static const boost::json::object kEmpty;
    const boost::json::object& o =
        value.is_object() ? value.get_object() : kEmpty;

    SongInfo s;
    s.name = model_detail::strCoalesce(o, "name", "title");
    s.subtitle = json::str(o, "subtitle");
    s.id = json::str(o, "id");
    s.duration = json::i64(o, "duration");
    s.mvId = json::str(o, "mv_id");
    s.album = model_detail::album(o.if_contains("album"));
    s.artists =
        model_detail::artists(model_detail::coalesce(o, "artists", "artist"));
    s.links = model_detail::links(o.if_contains("links"));
    std::string platform = json::str(o, "platform");
    s.platform =
        platform.empty() ? std::string(fallbackPlatform) : std::move(platform);
    s.cover = model_detail::cover(o);
    if (const auto* sub = o.if_contains("sublist")) {
        s.sublist = parseSongList(*sub, fallbackPlatform);
    }
    s.originalType = json::i64(o, "original_type");
    s.path = model_detail::nullableStr(o.if_contains("path"));
    s.size = model_detail::nullableInt(o.if_contains("size"));
    s.quality = model_detail::nullableStr(o.if_contains("quality"));
    s.alias = model_detail::nullableStr(o.if_contains("alias"));
    return s;
}

// `_songs`: array only; songs with an empty id or name are dropped.
inline std::vector<SongInfo> parseSongList(const boost::json::value& value,
                                           std::string_view fallbackPlatform) {
    std::vector<SongInfo> out;
    if (!value.is_array()) {
        return out;
    }
    for (const auto& item : value.get_array()) {
        SongInfo s = parseSongInfo(item, fallbackPlatform);
        if (!s.id.empty() && !s.name.empty()) {
            out.push_back(std::move(s));
        }
    }
    return out;
}

}  // namespace hemusic
