#include <SDK/foobar2000.h>
#include <SDK/component.h>
#include <SDK/componentversion.h>

// foo_hemusic — minimal foobar2000 v2 component scaffold (PLAN.md Phase 0).
// Registers version info + an initquit so foobar2000 lists the component and
// prints a load line to the console. No features yet.

#ifndef FOO_HEMUSIC_VERSION
#define FOO_HEMUSIC_VERSION "0.0.1"
#endif

// Mandatory since fb2k v1: lets the troubleshooter tell component versions apart.
DECLARE_COMPONENT_VERSION(
    "HE-Music",
    FOO_HEMUSIC_VERSION,
    "Brings the HE-Music backend into foobar2000 as a music source.\n");

// Prevents the DLL from being renamed / loaded in multiple copies.
VALIDATE_COMPONENT_FILENAME("foo_hemusic.dll");

namespace {

class hemusic_initquit : public initquit {
public:
    void on_init() override {
        console::print("foo_hemusic " FOO_HEMUSIC_VERSION " loaded.");
    }
    void on_quit() override {}
};

FB2K_SERVICE_FACTORY(hemusic_initquit);

} // namespace
