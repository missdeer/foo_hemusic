#include "core/session.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace hemusic {

Session& Session::instance() {
    static Session g;
    return g;
}

void Session::initialize(std::filesystem::path tokenPath, std::string baseUrl,
                         DeviceInfo device) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.emplace(std::move(tokenPath));
    baseUrl_ = std::move(baseUrl);
    device_ = std::move(device);
    tokens_ = store_->load();
    initialized_ = true;
    // A second initialize() (tests, profile-path change) must invalidate
    // any clients built before it -- they hold the prior token snapshot
    // and a refresh callback that would otherwise overwrite the new state.
    ++generation_;
}

bool Session::isInitialized() const {
    std::lock_guard<std::mutex> lock(mu_);
    return initialized_;
}

bool Session::isAuthenticated() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tokens_.has_value() && !tokens_->accessToken.empty();
}

std::optional<AuthTokenResult> Session::currentTokens() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tokens_;
}

bool Session::setTokens(const AuthTokenResult& tokens) {
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_ = tokens;
        // Hold the lock across the disk write so concurrent setTokens calls
        // serialize: in-memory and on-disk advance together, last writer wins
        // consistently in both places.
        if (store_) {
            saved = store_->save(tokens);
        }
    }
    notifyAuthListeners();
    return saved;
}

void Session::clearTokens() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_.reset();
        if (store_) {
            store_->clear();
        }
        // Any pre-clear ApiClient that's still mid-refresh would otherwise
        // resurrect the credential when it calls back into setTokens; the
        // generation guard in buildClient's callback short-circuits that.
        ++generation_;
    }
    notifyAuthListeners();
}

Session::AuthListenerId Session::addAuthListener(AuthListener cb) {
    std::lock_guard<std::mutex> lock(listenersMu_);
    const AuthListenerId id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(cb));
    return id;
}

void Session::removeAuthListener(AuthListenerId id) {
    std::lock_guard<std::mutex> lock(listenersMu_);
    std::erase_if(listeners_,
                  [id](const auto& entry) { return entry.first == id; });
}

void Session::notifyAuthListeners() {
    std::vector<AuthListener> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMu_);
        snapshot.reserve(listeners_.size());
        for (const auto& [id, cb] : listeners_) {
            snapshot.push_back(cb);
        }
    }
    for (const auto& cb : snapshot) {
        cb();
    }
}

std::optional<ApiClient> Session::buildClient(ApiClient::Transport transport) {
    AuthTokenResult snapshot;
    std::string baseUrl;
    DeviceInfo device;
    unsigned long long myGen = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) {
            return std::nullopt;
        }
        baseUrl = baseUrl_;
        device = device_;
        myGen = generation_;
        if (tokens_) {
            snapshot = *tokens_;
        }
    }
    ApiClient client(std::move(transport), std::move(baseUrl),
                     std::move(device));
    if (!snapshot.accessToken.empty()) {
        client.setTokens(snapshot.accessToken, snapshot.refreshToken,
                         snapshot.expiresAt);
    }
    client.setOnTokensRefreshed([this, myGen](const AuthTokenResult& fresh) {
        std::lock_guard<std::mutex> lock(mu_);
        // Drop refreshes from clients that pre-date a clearTokens() or a
        // re-initialize(); otherwise a 401 refresh that the user already
        // logged out of would silently rewrite a credential they cleared.
        if (myGen != generation_) {
            return;
        }
        tokens_ = fresh;
        if (store_) {
            store_->save(fresh);
        }
    });
    return client;
}

std::string Session::baseUrl() const {
    std::lock_guard<std::mutex> lock(mu_);
    return baseUrl_;
}

DeviceInfo Session::device() const {
    std::lock_guard<std::mutex> lock(mu_);
    return device_;
}

}  // namespace hemusic
