#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/oauth_flow.h"

// Persists the double-token credential (access_token + refresh_token +
// expires_at, api.md sec.0.6) DPAPI-encrypted on disk, so the token never lands
// in a plaintext cfg_var (CLAUDE.md). The bytes are scoped to the current
// Windows user (CryptProtectData), unreadable by other accounts.
//
// Layering keeps the pieces unit-testable: serialize/deserialize is pure JSON,
// dpapiProtect/Unprotect is a Win32 round-trip, and TokenStore composes them
// with file I/O. SDK-free so it lives in hemusic_core with the tests.

namespace hemusic {

// AuthTokenResult <-> JSON bytes. Stored shape:
//   {"access_token":..., "refresh_token":..., "expires_at":...}
std::string serializeToken(const AuthTokenResult& token);

// Parses serializeToken output; std::nullopt on malformed JSON or a missing
// access_token (an empty access token is not a usable credential).
std::optional<AuthTokenResult> deserializeToken(std::string_view json);

// DPAPI encrypt/decrypt for the current user (no extra entropy, no UI).
// std::nullopt when the Win32 call fails.
std::optional<std::vector<unsigned char>> dpapiProtect(
    const std::vector<unsigned char>& plain);
std::optional<std::vector<unsigned char>> dpapiUnprotect(
    const std::vector<unsigned char>& cipher);

// File-backed credential store at a caller-supplied path (the component derives
// it from the foobar2000 profile dir; tests pass a temp path).
class TokenStore {
   public:
    explicit TokenStore(std::filesystem::path path);

    // Serialize -> DPAPI-encrypt -> write (creating parent dirs). false on any
    // failure; never writes a partial credential the loader would accept.
    bool save(const AuthTokenResult& token) const;

    // Read -> DPAPI-decrypt -> parse. std::nullopt when absent, unreadable,
    // not decryptable (e.g. copied from another user), or malformed.
    std::optional<AuthTokenResult> load() const;

    // Removes the credential file (logout). true if the file is gone afterward
    // (including when it never existed).
    bool clear() const;

    const std::filesystem::path& path() const { return path_; }

   private:
    std::filesystem::path path_;
};

}  // namespace hemusic
