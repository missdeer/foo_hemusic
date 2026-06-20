#include "net/http_client.h"

#include <windows.h>
// clang-format off: winhttp.h must follow windows.h.
#include <winhttp.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace hemusic {

namespace {

constexpr size_t kHostBufferSize = 256;
constexpr size_t kUrlBufferSize = 2048;

std::wstring utf8ToWide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

// RAII wrapper for HINTERNET so each handle closes on scope exit.
class Handle {
   public:
    Handle() = default;
    explicit Handle(HINTERNET h) : h_(h) {}
    ~Handle() {
        if (h_ != nullptr) {
            WinHttpCloseHandle(h_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = delete;
    Handle& operator=(Handle&&) = delete;

    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

   private:
    HINTERNET h_ = nullptr;
};

// Builds a transport-failure response, capturing GetLastError immediately
// (must be the first call so no intervening Win32 work clobbers it).
HttpResponse transportFail(const char* what) {
    const DWORD code = GetLastError();
    HttpResponse r;
    r.ok = false;
    r.winError = code;
    r.error = what;
    return r;
}

bool hasHeader(const std::vector<std::pair<std::string, std::string>>& headers,
               std::string_view name) {
    return std::ranges::any_of(headers, [name](const auto& kv) {
        return kv.first.size() == name.size() &&
               _strnicmp(kv.first.c_str(), name.data(), name.size()) == 0;
    });
}

// Serializes the request headers into one CRLF-delimited block, injecting the
// Bearer token and a default JSON content-type unless the caller set them.
std::wstring buildHeaderBlock(const HttpRequest& req) {
    std::wstring headers;
    auto addHeader = [&headers](std::string_view name, std::string_view value) {
        // Drop any field whose name or value carries CR/LF: well-formed headers
        // never contain them, so this only fires on a corrupt/hostile value
        // (e.g. a tampered bearer token) and stops it splitting the request.
        if (name.find_first_of("\r\n") != std::string_view::npos ||
            value.find_first_of("\r\n") != std::string_view::npos) {
            return;
        }
        headers += utf8ToWide(name);
        headers += L": ";
        headers += utf8ToWide(value);
        headers += L"\r\n";
    };
    for (const auto& [key, value] : req.headers) {
        addHeader(key, value);
    }
    if (!req.bearerToken.empty() && !hasHeader(req.headers, "authorization")) {
        addHeader("authorization", "Bearer " + req.bearerToken);
    }
    if (!req.body.empty() && !hasHeader(req.headers, "content-type")) {
        addHeader("content-type", "application/json");
    }
    return headers;
}

// Drains the response stream into out. On failure fills err and returns false.
bool readResponseBody(HINTERNET request, std::string& out, HttpResponse& err) {
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
            err = transportFail("WinHttpQueryDataAvailable failed");
            return false;
        }
        if (available == 0) {
            break;
        }
        const size_t offset = out.size();
        out.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request, out.data() + offset, available, &read) ==
            FALSE) {
            err = transportFail("WinHttpReadData failed");
            return false;
        }
        out.resize(offset + read);
        if (read == 0) {
            break;
        }
    }
    return true;
}

}  // namespace

std::optional<ParsedUrl> crackUrl(const std::string& url) {
    const std::wstring wurl = utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    std::array<wchar_t, kHostBufferSize> host{};
    std::array<wchar_t, kUrlBufferSize> path{};
    std::array<wchar_t, kUrlBufferSize> extra{};
    uc.lpszHostName = host.data();
    uc.dwHostNameLength = static_cast<DWORD>(host.size());
    uc.lpszUrlPath = path.data();
    uc.dwUrlPathLength = static_cast<DWORD>(path.size());
    uc.lpszExtraInfo = extra.data();
    uc.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0,
                        &uc) == FALSE) {
        return std::nullopt;
    }

    ParsedUrl out;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.port =
        uc.nPort;  // WinHttpCrackUrl fills the scheme default when absent
    out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out.path.empty()) {
        out.path = L"/";
    }
    return out;
}

HttpClient::HttpClient(const std::wstring& userAgent)
    : session_(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
}

HttpClient::~HttpClient() {
    if (session_ != nullptr) {
        WinHttpCloseHandle(static_cast<HINTERNET>(session_));
    }
}

HttpResponse HttpClient::send(const HttpRequest& req) {
    if (session_ == nullptr) {
        return transportFail("WinHttpOpen failed");
    }

    const auto parsed = crackUrl(req.url);
    if (!parsed) {
        HttpResponse r;
        r.error = "invalid URL";
        return r;
    }

    Handle conn(WinHttpConnect(static_cast<HINTERNET>(session_),
                               parsed->host.c_str(),
                               static_cast<INTERNET_PORT>(parsed->port), 0));
    if (!conn) {
        return transportFail("WinHttpConnect failed");
    }

    const wchar_t* verb = (req.method == HttpMethod::Post) ? L"POST" : L"GET";
    const DWORD secureFlag = parsed->https ? WINHTTP_FLAG_SECURE : 0;
    Handle request(WinHttpOpenRequest(
        conn.get(), verb, parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secureFlag));
    if (!request) {
        return transportFail("WinHttpOpenRequest failed");
    }

    WinHttpSetTimeouts(request.get(), req.connectTimeoutMs,
                       req.connectTimeoutMs, req.connectTimeoutMs,
                       req.readTimeoutMs);

    const std::wstring headers = buildHeaderBlock(req);
    const wchar_t* headerPtr =
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
    const DWORD headerLen = headers.empty() ? 0 : static_cast<DWORD>(-1);
    // WinHttpSendRequest does not modify lpOptional; const_cast is the
    // documented idiom for passing a read-only body.
    void* bodyPtr = WINHTTP_NO_REQUEST_DATA;
    if (!req.body.empty()) {
        bodyPtr = const_cast<char*>(req.body.data());  // NOLINT
    }
    const auto bodyLen = static_cast<DWORD>(req.body.size());

    if (WinHttpSendRequest(request.get(), headerPtr, headerLen, bodyPtr,
                           bodyLen, bodyLen, 0) == FALSE) {
        return transportFail("WinHttpSendRequest failed");
    }
    if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        return transportFail("WinHttpReceiveResponse failed");
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return transportFail("WinHttpQueryHeaders failed");
    }

    HttpResponse out;
    if (!readResponseBody(request.get(), out.body, out)) {
        return out;
    }
    out.ok = true;
    out.status = static_cast<long>(status);
    return out;
}

}  // namespace hemusic
