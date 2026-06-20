#include "auth/token_store.h"

#include <windows.h>
#include <dpapi.h>

#include <fstream>
#include <iterator>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "net/json_codec.h"

namespace hemusic {

namespace {

std::vector<unsigned char> toBytes(std::string_view s) {
    return {s.begin(), s.end()};
}

std::optional<std::vector<unsigned char>> readFile(
    const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
}

// Writes via a sibling temp file + rename so a failed/partial write can never
// clobber an existing valid credential: the old file stays intact until the
// fully-written replacement atomically takes its place (same volume).
bool writeFileAtomic(const std::filesystem::path& path,
                     const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

std::string serializeToken(const AuthTokenResult& token) {
    return boost::json::serialize(boost::json::object{
        {"access_token", token.accessToken},
        {"refresh_token", token.refreshToken},
        {"expires_at", token.expiresAt},
    });
}

std::optional<AuthTokenResult> deserializeToken(std::string_view json) {
    boost::system::error_code ec;
    auto value = boost::json::parse(json, ec);
    if (ec || !value.is_object()) {
        return std::nullopt;
    }
    const auto& obj = value.get_object();
    std::string access = hemusic::json::str(obj, "access_token");
    if (access.empty()) {
        return std::nullopt;
    }
    return AuthTokenResult{
        std::move(access),
        hemusic::json::str(obj, "refresh_token"),
        hemusic::json::i64(obj, "expires_at"),
    };
}

std::optional<std::vector<unsigned char>> dpapiProtect(
    const std::vector<unsigned char>& plain) {
    std::vector<unsigned char> input =
        plain;  // DATA_BLOB wants a mutable buffer
    DATA_BLOB in{static_cast<DWORD>(input.size()), input.data()};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"foo_hemusic token", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    std::vector<unsigned char> cipher(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return cipher;
}

std::optional<std::vector<unsigned char>> dpapiUnprotect(
    const std::vector<unsigned char>& cipher) {
    std::vector<unsigned char> input =
        cipher;  // DATA_BLOB wants a mutable buffer
    DATA_BLOB in{static_cast<DWORD>(input.size()), input.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    std::vector<unsigned char> plain(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return plain;
}

TokenStore::TokenStore(std::filesystem::path path) : path_(std::move(path)) {}

bool TokenStore::save(const AuthTokenResult& token) const {
    auto cipher = dpapiProtect(toBytes(serializeToken(token)));
    if (!cipher) {
        return false;
    }
    return writeFileAtomic(path_, *cipher);
}

std::optional<AuthTokenResult> TokenStore::load() const {
    auto cipher = readFile(path_);
    if (!cipher) {
        return std::nullopt;
    }
    auto plain = dpapiUnprotect(*cipher);
    if (!plain) {
        return std::nullopt;
    }
    return deserializeToken(std::string_view(
        reinterpret_cast<const char*>(plain->data()), plain->size()));
}

bool TokenStore::clear() const {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    return !ec && !std::filesystem::exists(path_, ec);
}

}  // namespace hemusic
