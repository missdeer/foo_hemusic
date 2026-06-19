#include "auth/oauth_flow.h"

#include "net/json_codec.h"

namespace hemusic {

namespace {

// Returns the trimmed legacy "token" string from a {token} or {data:{token}}
// envelope, or "" when neither is present (api.md sec.0.6, Flutter _findToken).
std::string findLegacyToken(const boost::json::object& obj) {
    if (const auto* direct = obj.if_contains("token")) {
        return json::toStr(*direct);
    }
    if (const auto* data = obj.if_contains("data"); data && data->is_object()) {
        if (const auto* nested = data->get_object().if_contains("token")) {
            return json::toStr(*nested);
        }
    }
    return {};
}

}  // namespace

AuthStatus AuthStatusResult::classifyAuthStatus(std::string_view status) {
    if (status == "pending") {
        return AuthStatus::Pending;
    }
    if (status == "success") {
        return AuthStatus::Success;
    }
    if (status == "failed") {
        return AuthStatus::Failed;
    }
    if (status == "expired") {
        return AuthStatus::Expired;
    }
    if (status == "error") {
        return AuthStatus::Error;
    }
    return AuthStatus::Unknown;
}

boost::json::object buildSessionRequest(const std::string& provider,
                                        const std::string& redirectUri,
                                        const DeviceInfo& device) {
    boost::json::object body{
        {"provider", provider},
        {"device_info", device.toApiObject()},
    };
    // Omit redirect_uri unless non-empty so the backend uses its default
    // callback (Flutter never sends it for linuxdo).
    if (!redirectUri.empty()) {
        body.emplace("redirect_uri", redirectUri);
    }
    return body;
}

boost::json::object buildRefreshRequest(const std::string& refreshToken,
                                        const DeviceInfo& device) {
    return boost::json::object{
        {"refresh_token", refreshToken},
        {"device_info", device.toApiObject()},
    };
}

std::vector<std::string> parseAuthProviders(const boost::json::value& body) {
    std::vector<std::string> out;
    const auto obj = json::asObject(body);
    const auto* list = obj.if_contains("list");
    if (list == nullptr || !list->is_array()) {
        return out;
    }
    for (const auto& item : list->get_array()) {
        std::string id = json::toStr(item);
        if (!id.empty()) {
            out.push_back(std::move(id));
        }
    }
    return out;
}

AuthCodeUrlResult parseAuthCodeUrl(const boost::json::value& body) {
    const auto obj = json::asObject(body);
    return AuthCodeUrlResult{
        json::str(obj, "url"),
        json::str(obj, "state"),
        static_cast<int>(json::i64(obj, "check_interval")),
        json::i64(obj, "expires_at"),
    };
}

AuthStatusResult parseAuthStatus(const boost::json::value& body) {
    const auto obj = json::asObject(body);
    return AuthStatusResult{
        json::str(obj, "status"),
        json::str(obj, "error"),
        static_cast<int>(json::i64(obj, "check_interval")),
        json::i64(obj, "expires_at"),
    };
}

AuthTokenResult parseAuthToken(const boost::json::value& body) {
    const auto obj = json::asObject(body);
    std::string access = json::str(obj, "access_token");
    if (access.empty()) {
        access = findLegacyToken(obj);
    }
    return AuthTokenResult{
        std::move(access),
        json::str(obj, "refresh_token"),
        json::i64(obj, "expires_at"),
    };
}

}  // namespace hemusic
