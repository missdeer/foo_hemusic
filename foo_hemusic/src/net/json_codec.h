#pragma once

#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>

#include <boost/json.hpp>

// Lenient JSON field access mirroring HE-Music-Flutter's OnlineApiClient
// helpers (`_asMap` / `_asInt` / `'${v ?? ""}'.trim()`). The backend is loose
// with types (numbers-as-strings, missing keys), so the whole api/ layer reads
// fields through these instead of strict boost::json accessors.

namespace hemusic::json {

// Stringify a scalar the way Flutter's `'$value'` does, then trim. Strings are
// trimmed; numbers/bools render to their text form; null/array/object -> "".
inline std::string toStr(const boost::json::value& v) {
    auto trim = [](std::string_view s) -> std::string {
        const char* ws = " \t\r\n";
        const auto b = s.find_first_not_of(ws);
        if (b == std::string_view::npos) {
            return {};
        }
        const auto e = s.find_last_not_of(ws);
        return std::string(s.substr(b, e - b + 1));
    };
    switch (v.kind()) {
        case boost::json::kind::string: {
            const auto& gs = v.get_string();
            return trim(std::string_view(gs.c_str(), gs.size()));
        }
        case boost::json::kind::int64:
            return std::to_string(v.get_int64());
        case boost::json::kind::uint64:
            return std::to_string(v.get_uint64());
        case boost::json::kind::double_:
            return trim(std::to_string(v.get_double()));
        case boost::json::kind::bool_:
            return v.get_bool() ? "true" : "false";
        default:
            return {};
    }
}

// Coerce a scalar to int64; numeric strings are parsed (full match, else 0),
// matching Flutter's `value is int ? value : int.tryParse('$value') ?? 0`.
inline long long toI64(const boost::json::value& v) {
    switch (v.kind()) {
        case boost::json::kind::int64:
            return v.get_int64();
        case boost::json::kind::uint64:
            return static_cast<long long>(v.get_uint64());
        case boost::json::kind::double_:
            return static_cast<long long>(v.get_double());
        case boost::json::kind::string: {
            const auto& s = v.get_string();
            long long out = 0;
            const char* begin = s.c_str();
            const char* end = begin + s.size();
            auto [ptr, ec] = std::from_chars(begin, end, out);
            return (ec == std::errc{} && ptr == end) ? out : 0;
        }
        default:
            return 0;
    }
}

// Trimmed string value at key; "" when absent. obj must be an object.
inline std::string str(const boost::json::object& obj, std::string_view key) {
    const auto* p = obj.if_contains(key);
    return p ? toStr(*p) : std::string{};
}

// First non-empty trimmed string among the given keys; "" if none match.
// Mirrors the Flutter detail clients' `for (key in keys) { ... isNotEmpty }`.
inline std::string firstNonEmpty(const boost::json::object& obj,
                                 std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v = str(obj, k);
        if (!v.empty()) {
            return v;
        }
    }
    return {};
}

// int64 value at key; 0 when absent.
inline long long i64(const boost::json::object& obj, std::string_view key) {
    const auto* p = obj.if_contains(key);
    return p ? toI64(*p) : 0;
}

// Coerce any value into an object: passes objects through, everything else
// becomes empty. Mirrors Flutter `_asMap` for the "expected an object" cases.
inline boost::json::object asObject(const boost::json::value& v) {
    return v.is_object() ? v.get_object() : boost::json::object{};
}

}  // namespace hemusic::json
