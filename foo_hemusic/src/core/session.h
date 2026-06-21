#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "auth/device_info.h"
#include "auth/oauth_flow.h"
#include "auth/token_store.h"
#include "net/api_client.h"

// Process-wide credential / session state, SDK-free so it lives in
// hemusic_core and unit-tests can exercise it. Owns:
//
//   1. The on-disk DPAPI token (via TokenStore) -- single writer.
//   2. The current in-memory AuthTokenResult snapshot -- single source of
//      truth for "am I logged in?".
//   3. The immutable session inputs every typed API call needs: base URL +
//      device info.
//
// Use:
//   - main thread, component init: Session::instance().initialize(...).
//   - any thread, before sending requests: buildClient(transport).
//   - any thread, on login / logout / 401 refresh: setTokens / clearTokens.
//
// initialize() captures profile-derived inputs from the caller (config:: in
// the component DLL, a temp dir in tests) so Session doesn't reach back into
// fb2k itself and can be exercised without it.

namespace hemusic {

class Session {
   public:
    static Session& instance();

    // Captures session inputs + tries to load a previously saved credential.
    // Idempotent: a second call replaces previous inputs and reloads. Main
    // thread expected (matches component init).
    void initialize(std::filesystem::path tokenPath, std::string baseUrl,
                    DeviceInfo device);

    // True once initialize() has run (regardless of whether a credential is
    // present). Cheap, thread-safe.
    bool isInitialized() const;

    // True iff a non-empty access token is currently held.
    bool isAuthenticated() const;

    // Snapshot of the current credential (by value, safe to keep). nullopt
    // when none is held.
    std::optional<AuthTokenResult> currentTokens() const;

    // Replaces in-memory credential + persists to disk + (TODO when added)
    // notifies listeners. Returns true on successful disk write; returns
    // false on disk failure (in-memory state is still updated -- the
    // credential is valid for this run, just not for the next).
    bool setTokens(const AuthTokenResult& tokens);

    // Clears the credential, both in-memory and on disk. Idempotent.
    void clearTokens();

    // Builds an ApiClient pre-loaded with the current credential + wired so
    // that any 401 refresh inside ApiClient::send is mirrored back into
    // Session (and therefore persisted). Caller owns the returned client;
    // if Session credentials change afterwards the client is stale and
    // should be rebuilt before further requests.
    //
    // Returns nullopt when Session hasn't been initialize()d.
    std::optional<ApiClient> buildClient(ApiClient::Transport transport);

    // Immutable session inputs captured by initialize().
    std::string baseUrl() const;
    DeviceInfo device() const;

   private:
    Session() = default;

    mutable std::mutex mu_;
    bool initialized_ = false;
    std::optional<TokenStore> store_;
    std::optional<AuthTokenResult> tokens_;
    std::string baseUrl_;
    DeviceInfo device_;
    // Bumped by clearTokens() / re-initialize so a stale onTokensRefreshed
    // callback fired by an old ApiClient (that completed a 401 refresh
    // after the user logged out) can't resurrect the credential on disk.
    // buildClient captures the live value; the callback only writes when
    // the value still matches.
    unsigned long long generation_ = 0;
};

}  // namespace hemusic
