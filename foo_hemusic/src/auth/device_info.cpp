#include "auth/device_info.h"

#include <windows.h>
#include <rpc.h>

#include <string>

namespace hemusic {

boost::json::object DeviceInfo::toApiObject() const {
    return boost::json::object{
        {"device_id", deviceId},
        {"platform", platform},
        {"app_type", appType},
        {"app_version", appVersion},
        {"device_name", deviceName},
    };
}

DeviceInfo makeDeviceInfo(std::string deviceId, std::string deviceName,
                          std::string appVersion) {
    return DeviceInfo{
        std::move(deviceId),
        "windows",
        "flutter",
        std::move(appVersion),
        std::move(deviceName),
    };
}

std::string newDeviceId() {
    UUID uuid{};
    RPC_CSTR str = nullptr;
    std::string id = "flutter_windows_";
    if (UuidCreate(&uuid) == RPC_S_OK &&
        UuidToStringA(&uuid, &str) == RPC_S_OK && str != nullptr) {
        id += reinterpret_cast<const char*>(str);
        RpcStringFreeA(&str);
    }
    return id;
}

std::string queryComputerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &len) || len == 0) {
        return "windows";
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                                    nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return "windows";
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), out.data(),
                        bytes, nullptr, nullptr);
    return out;
}

}  // namespace hemusic
