#include "auth/device_info.h"

#include <regex>

#include <catch2/catch_test_macros.hpp>

using hemusic::makeDeviceInfo;
using hemusic::newDeviceId;

// The backend identifies clients by this map; the fields and the Flutter
// masquerade values are the contract (api.md §0.7), not incidental strings.
TEST_CASE("toApiObject emits the Flutter device_info contract", "[device_info]") {
    auto info = makeDeviceInfo("flutter_windows_abc", "MY-PC", "0.0.1");
    auto obj = info.toApiObject();

    // Exactly the five proto DeviceInfo keys, no more.
    REQUIRE(obj.size() == 5);
    REQUIRE(obj.at("device_id").as_string() == "flutter_windows_abc");
    REQUIRE(obj.at("device_name").as_string() == "MY-PC");
    REQUIRE(obj.at("app_version").as_string() == "0.0.1");

    // Masquerade values are fixed regardless of caller input.
    REQUIRE(obj.at("platform").as_string() == "windows");
    REQUIRE(obj.at("app_type").as_string() == "flutter");
}

TEST_CASE("newDeviceId is Flutter-shaped: flutter_windows_<uuid>", "[device_info]") {
    const std::string id = newDeviceId();
    static const std::regex shape(
        R"(^flutter_windows_[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-)"
        R"([0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
    REQUIRE(std::regex_match(id, shape));
}
