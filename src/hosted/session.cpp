#include <winsock2.h>

#include "app/log.h"
#include "hosted/session.h"
#include <algorithm>
#include <condition_variable>
#include <utility>

namespace tmloader::hosted
{
namespace
{

using namespace std::chrono_literals;

// Refresh an access token this long before it expires, and renew play this long before it lapses.
constexpr auto kAccessMargin = 60s;
constexpr auto kRenewMargin  = 300s;

void startWinsock()
{
    static const auto started = []
    {
        auto data = WSADATA{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started)
    {
        throw std::runtime_error("Cannot initialize networking");
    }
}

auto isInvalidToken(const ApiError& error) -> bool
{
    return error.status() == 401 && error.code() == "invalid_token";
}

// Returns false when a stop is requested before the deadline.
auto sleepUntil(const std::stop_token& stop, std::chrono::steady_clock::time_point deadline) -> bool
{
    auto mutex = std::mutex{};
    auto wake  = std::condition_variable_any{};
    auto lock  = std::unique_lock(mutex);
    wake.wait_until(lock, stop, deadline, []
                    {
                        return false;
                    });
    return !stop.stop_requested();
}

} // namespace

HostedSession::HostedSession(const Config& config)
: origin_(config.hostedServer)
, http_(config.insecureLoopback)
, auth_(http_, origin_)
, store_(origin_, auth_.settings().supabaseUrl)
{
    startWinsock();
}

HostedSession::~HostedSession()
{
    endPlay();
}

auto HostedSession::resume() -> bool
{
    const auto remembered = store_.load();
    if (!remembered)
    {
        return false;
    }

    try
    {
        const std::lock_guard lock(tokenMutex_);
        remember(auth_.refresh(*remembered));
        return true;
    }
    catch (const ApiError& error)
    {
        logLine("Remembered sign-in rejected ({})", error.status());
        store_.erase();
        return false;
    }
}

auto HostedSession::signIn(const OpenBrowser& openBrowser, const std::stop_token& stop) -> bool
{
    auto tokens = auth_.signIn(openBrowser, stop);
    if (!tokens)
    {
        return false;
    }

    const std::lock_guard lock(tokenMutex_);
    remember(std::move(*tokens));
    return true;
}

auto HostedSession::signedIn() -> bool
{
    const std::lock_guard lock(tokenMutex_);
    return tokens_.has_value();
}

void HostedSession::logout() noexcept
{
    endPlay();
    store_.erase();
    const std::lock_guard lock(tokenMutex_);
    tokens_.reset();
}

auto HostedSession::profile() -> Profile
{
    const std::lock_guard lock(tokenMutex_);
    if (!tokens_)
    {
        return {};
    }

    return { tokens_->userId, tokens_->displayName, tokens_->avatarUrl };
}

auto HostedSession::avatar() -> std::string
{
    const auto url = profile().avatarUrl;
    if (!url.starts_with("https://"))
    {
        return {};
    }

    try
    {
        const auto response = http_.send("GET", url);
        if (response.status == 200)
        {
            return response.body;
        }
    }
    catch (const std::exception& error)
    {
        logLine("Avatar download failed: {}", error.what());
    }

    return {};
}

void HostedSession::remember(Tokens tokens)
{
    if (!tokens.userId.empty() && (!tokens_ || tokens_->userId != tokens.userId))
    {
        logLine("Signed in as Supabase user {}", tokens.userId);
    }

    store_.save(tokens.refresh);
    tokens_ = std::move(tokens);
}

auto HostedSession::accessToken(bool forceRefresh) -> std::string
{
    // Refreshes are serialized; refresh tokens rotate and both are replaced together.
    const std::lock_guard lock(tokenMutex_);
    if (!tokens_)
    {
        throw ApiError(401, "invalid_token");
    }

    if (forceRefresh || std::chrono::system_clock::now() + kAccessMargin >= tokens_->expiresAt)
    {
        try
        {
            remember(auth_.refresh(tokens_->refresh));
        }
        catch (const ApiError&)
        {
            tokens_.reset();
            store_.erase();
            throw ApiError(401, "invalid_token");
        }
    }

    return tokens_->access;
}

void HostedSession::withAccess(const std::function<void(const std::string&)>& call)
{
    try
    {
        call(accessToken());
    }
    catch (const ApiError& error)
    {
        if (!isInvalidToken(error))
        {
            throw;
        }

        call(accessToken(true));
    }
}

void HostedSession::startPlay()
{
    auto ticket = std::string{};
    withAccess([&](const std::string& access)
               {
                   ticket = auth_.issueTicket(access);
               });
    play_    = auth_.redeem(ticket);
    tunnel_  = std::make_unique<Tunnel>(http_, Tunnel::Streams{ play_->gameStream, play_->profileStream, play_->token, auth_.settings().subprotocol });
    renewal_ = std::jthread([this](const std::stop_token& stop)
                            {
                                renewLoop(stop);
                            });
    logLine("Hosted play session started");
}

void HostedSession::disconnectOtherSession()
{
    withAccess([&](const std::string& access)
               {
                   auth_.revokeAccountSession(access);
               });
}

void HostedSession::endPlay() noexcept
{
    if (renewal_.joinable())
    {
        renewal_.request_stop();
        renewal_.join();
    }

    if (play_)
    {
        try
        {
            auth_.revokeSession(play_->token);
        }
        catch (const std::exception& error)
        {
            logLine("Play session revoke failed: {}", error.what());
        }

        play_.reset();
    }

    tunnel_.reset();
}

auto HostedSession::gameConfig(Config config) const -> Config
{
    config.server      = "127.0.0.1";
    config.gamePort    = tunnel_->gamePort();
    config.profilePort = tunnel_->profilePort();
    config.polId       = play_->polId;
    config.password    = play_->password;
    return config;
}

void HostedSession::renewLoop(const std::stop_token& stop)
{
    auto expiry = std::chrono::steady_clock::now() + play_->expiresIn;
    auto next   = expiry - kRenewMargin;
    auto retry  = std::chrono::seconds(15s);
    while (sleepUntil(stop, next))
    {
        try
        {
            withAccess([&](const std::string& access)
                       {
                           auth_.renew(access, play_->token);
                       });
            expiry = std::chrono::steady_clock::now() + play_->expiresIn;
            next   = expiry - kRenewMargin;
            retry  = 15s;
            logLine("Hosted play session renewed");
            continue;
        }
        catch (const ApiError& error)
        {
            logLine("Play session renewal failed ({} {})", error.status(), error.code());
            // Only an unavailable service is worth retrying; anything else ends play.
            if (error.status() != 503)
            {
                break;
            }
        }
        catch (const std::exception& error)
        {
            logLine("Play session renewal failed: {}", error.what());
        }

        next  = std::chrono::steady_clock::now() + retry;
        retry = std::min<std::chrono::seconds>(retry * 2, 120s);
        if (next >= expiry)
        {
            break;
        }
    }

    if (!stop.stop_requested())
    {
        tunnel_->closeConnections();
    }
}

} // namespace tmloader::hosted
