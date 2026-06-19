#pragma once

#include <string>

#include <boost/json.hpp>

// device_info payload sent on login / OAuth / token-refresh requests.
// Mirrors HE-Music-Flutter `DeviceInfoData.toApiMap()` (api.md §0.7).
// Client identity masquerades as the Flutter app (app_type="flutter",
// device_id="flutter_windows_<uuid>") to stay compatible with the backend.

namespace hemusic {

struct DeviceInfo {
    std::string deviceId;    // "flutter_windows_<uuid>"
    std::string platform;    // "windows"
    std::string appType;     // "flutter"
    std::string appVersion;  // component version, e.g. "0.0.1"
    std::string deviceName;  // computer name

    boost::json::object toApiObject() const;
};

// Pure assembly: fills the fixed platform/appType, leaves caller-owned fields.
DeviceInfo makeDeviceInfo(std::string deviceId, std::string deviceName,
                          std::string appVersion);

// Generates a fresh, Flutter-shaped device id: "flutter_windows_<uuid>".
// Persistence is the caller's job (cfg_var), matching Flutter's prefs flow.
std::string newDeviceId();

// Current machine name via GetComputerNameW; falls back to "windows".
std::string queryComputerName();

}  // namespace hemusic
