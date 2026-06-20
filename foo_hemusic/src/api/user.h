#pragma once

#include <cstdint>
#include <string>

#include <boost/json.hpp>

#include "net/json_codec.h"

// GET /v1/user/info response model + parser (api.md sec.1.2). Mirrors
// HE-Music-Flutter `MyOverviewApiClient.fetchProfile` / `MyProfile`: string
// fields stringify-then-trim, `status` tolerates string-encoded ints. Fields
// are top-level (Flutter reads response.data directly, no envelope).
// Header-only like the rest of the api/ layer.

namespace hemusic {

struct UserInfo {
    std::string id;
    std::string username;
    std::string nickname;
    std::string email;
    long long status = 0;
    std::string avatarUrl;  // "avatar" field

    // A profile is usable (token verified) only with an identity key.
    bool valid() const { return !id.empty(); }
};

inline UserInfo parseUserInfo(const boost::json::value& body) {
    const auto obj = json::asObject(body);
    return UserInfo{
        json::str(obj, "id"),       json::str(obj, "username"),
        json::str(obj, "nickname"), json::str(obj, "email"),
        json::i64(obj, "status"),   json::str(obj, "avatar"),
    };
}

}  // namespace hemusic
