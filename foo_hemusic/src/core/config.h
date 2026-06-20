#pragma once

#include <filesystem>
#include <string>

// Component configuration backed by foobar2000 cfg_var (SDK-bound, so this
// lives in the component target, not hemusic_core). Holds the persistent device
// id and the user-tunable backend base URL, and derives the on-disk token path
// from the fb2k profile directory. The DPAPI-encrypted token itself is handled
// by TokenStore -- this only decides *where* it goes.
//
// Call these on a thread where core_api services are available (not during
// static init / shutdown).

namespace hemusic::config {

// Flutter-shaped device id ("flutter_windows_<uuid>"), generated once on first
// access and persisted (cfg_string). Stable across restarts thereafter.
std::string deviceId();

// HE-Music backend base URL (cfg_string, default "https://y.wjhe.top").
// Trimmed; falls back to the default when the user has cleared it.
std::string apiBaseUrl();
void setApiBaseUrl(const std::string& url);

// Native path to the DPAPI token file: <fb2k profile>/foo_hemusic/token.bin.
// Profile-rooted (not the component install dir) so it survives reinstalls.
std::filesystem::path tokenStorePath();

}  // namespace hemusic::config
