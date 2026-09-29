#pragma once
#include "config/config.h"
#include "hosted/auth.h"
#include "hosted/tunnel.h"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace tmloader::hosted
{

// Hosted play for one launch: sign-in state, the play session, its renewal and the tunnel.
class HostedSession
{
public:
    explicit HostedSession(const Config& config);
    ~HostedSession();
    HostedSession(const HostedSession&)                    = delete;
    auto operator=(const HostedSession&) -> HostedSession& = delete;
    HostedSession(HostedSession&&)                         = delete;
    auto operator=(HostedSession&&) -> HostedSession&      = delete;

    // Signs in silently with a remembered login; false when a browser sign-in is needed.
    auto resume() -> bool;
    auto signIn(const OpenBrowser& openBrowser, std::stop_token stop) -> bool;
    auto signedIn() -> bool;
    // The signed-in Supabase user UUID, for enrollment by the server operator.
    auto userId() -> std::string;
    // Revokes play, forgets the remembered login and signs out locally.
    void logout() noexcept;

    // Creates the play session and starts forwarding. Throws ApiError on refusal (e.g. 409 session_conflict).
    void startPlay();
    void disconnectOtherSession();
    // Revokes the play session and closes forwarded connections.
    void endPlay() noexcept;

    // Game configuration once play has started: loopback tunnel ports and the returned legacy account.
    auto gameConfig(Config config) const -> Config;

private:
    auto accessToken(bool forceRefresh = false) -> std::string;
    auto withAccess(const std::function<void(const std::string&)>& call) -> void;
    void remember(Tokens tokens);
    void renewLoop(std::stop_token stop);

    std::string                    origin_;
    HttpClient                     http_;
    AuthClient                     auth_;
    std::optional<CredentialStore> store_;
    std::mutex                     tokenMutex_;
    std::optional<Tokens>          tokens_;
    std::optional<PlaySession>     play_;
    std::unique_ptr<Tunnel>        tunnel_;
    std::jthread                   renewal_;
};

} // namespace tmloader::hosted
