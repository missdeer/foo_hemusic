#include <SDK/foobar2000.h>
#include <SDK/component.h>
#include <SDK/componentversion.h>

#include "auth/device_info.h"
#include "core/config.h"
#include "core/session.h"
#include "ui/cover_cache.h"

// foo_hemusic — minimal foobar2000 v2 component scaffold (PLAN.md Phase 0).
// Registers version info + an initquit so foobar2000 lists the component and
// prints a load line to the console.

// Macro (not constexpr) because both DECLARE_COMPONENT_VERSION and the
// console::print() banner rely on adjacent string-literal concatenation.
#ifndef FOO_HEMUSIC_VERSION
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FOO_HEMUSIC_VERSION "0.0.1"
#endif

// Mandatory since fb2k v1: lets the troubleshooter tell component versions
// apart.
DECLARE_COMPONENT_VERSION(
    "HE-Music", FOO_HEMUSIC_VERSION,
    "Brings the HE-Music backend into foobar2000 as a music source.\n");

// Prevents the DLL from being renamed / loaded in multiple copies.
VALIDATE_COMPONENT_FILENAME("foo_hemusic.dll");

namespace {

class hemusic_initquit : public initquit {
   public:
    void on_init() override {
        // Wire the session: load any previously-saved credential from the
        // profile-rooted DPAPI file, snapshot the immutable inputs (base URL +
        // device info) every typed API call needs. Main thread, so cfg_var
        // and core_api calls are safe here.
        hemusic::Session::instance().initialize(
            hemusic::config::tokenStorePath(), hemusic::config::apiBaseUrl(),
            hemusic::makeDeviceInfo(hemusic::config::deviceId(),
                                    hemusic::queryComputerName(),
                                    FOO_HEMUSIC_VERSION));
        // Owns the shared cover ImageCache here (not a function-local static)
        // so its worker pool is stopped + joined in on_quit, on the main
        // thread, before the DLL detaches and the d2d/WIC statics tear down.
        hemusic::ui::initCoverCache();
        console::print("foo_hemusic " FOO_HEMUSIC_VERSION " loaded.");
    }
    void on_quit() override { hemusic::ui::shutdownCoverCache(); }
};

FB2K_SERVICE_FACTORY(hemusic_initquit);

}  // namespace
