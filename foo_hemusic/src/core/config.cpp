#include "core/config.h"

#include <SDK/foobar2000.h>

#include <mutex>
#include <string>
#include <string_view>

#include "auth/device_info.h"

namespace hemusic::config {

namespace {

// cfg_var identities -- random GUIDs, must never be reused for another var.
// {2AEA602F-3F3E-4447-889B-22D23BC0108A}
constexpr GUID kGuidDeviceId = {
    0x2aea602f,
    0x3f3e,
    0x4447,
    {0x88, 0x9b, 0x22, 0xd2, 0x3b, 0xc0, 0x10, 0x8a}};
// {9F9392FF-7C87-4D02-BD66-F5A8B39E82A8}
constexpr GUID kGuidApiBaseUrl = {
    0x9f9392ff,
    0x7c87,
    0x4d02,
    {0xbd, 0x66, 0xf5, 0xa8, 0xb3, 0x9e, 0x82, 0xa8}};

constexpr const char* kDefaultBaseUrl = "https://y.wjhe.top";

cfg_string g_deviceId(kGuidDeviceId, "");
cfg_string g_apiBaseUrl(kGuidApiBaseUrl, kDefaultBaseUrl);

std::string trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(ws);
    return std::string(s.substr(b, e - b + 1));
}

std::string cfgTrimmed(cfg_string& var) {
    const pfc::string8& value = var.get();  // lifetime-extended temporary
    return trim(std::string_view(value.c_str(), value.length()));
}

}  // namespace

std::string deviceId() {
    // Serialize the read-generate-persist so two concurrent first calls can't
    // mint and store two different ids (last-writer-wins divergence).
    static std::mutex mtx;
    const std::lock_guard<std::mutex> lock(mtx);
    std::string id = cfgTrimmed(g_deviceId);
    if (id.empty()) {
        id = newDeviceId();
        g_deviceId.set(id.c_str());
    }
    return id;
}

std::string apiBaseUrl() {
    std::string url = cfgTrimmed(g_apiBaseUrl);
    return url.empty() ? kDefaultBaseUrl : url;
}

void setApiBaseUrl(const std::string& url) { g_apiBaseUrl.set(url.c_str()); }

std::filesystem::path tokenStorePath() {
    const char* profile = core_api::get_profile_path();  // e.g. "file://C:\..."
    pfc::string8 native;
    std::string base;
    if (extract_native_path(profile, native) && native.length() > 0) {
        base.assign(native.c_str(), native.length());
    } else {
        // Never resolve relative to the CWD: strip the file:// scheme ourselves
        // so the token still lands under an absolute profile path.
        std::string_view p(profile);
        constexpr std::string_view kFilePrefix = "file://";
        if (p.starts_with(kFilePrefix)) {
            p.remove_prefix(kFilePrefix.size());
        }
        base.assign(p);
    }
    std::filesystem::path dir(std::u8string(
        reinterpret_cast<const char8_t*>(base.data()), base.size()));
    return dir / "foo_hemusic" / "token.bin";
}

}  // namespace hemusic::config
