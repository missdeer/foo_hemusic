#pragma once

// Temporary Phase 1 login UI: a modeless Win32 dialog that drives the OAuth
// login flow (login_flow::runLogin) on a worker thread, shows progress, saves
// the token on success and prints the HE-Music username. Thrown away once the
// real Default-UI login dialog lands in Phase 5.

namespace hemusic::ui {

// Opens the login dialog. Must be called on the main thread (e.g. from a
// mainmenu command). If a dialog is already open, brings it to the foreground
// instead of opening a second one.
void showLoginDialog();

}  // namespace hemusic::ui
