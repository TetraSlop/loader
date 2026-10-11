#include "hosted/session.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <thread>

// Hidden: needs tests/hosted/gateway_harness.py to provide a live gateway.
using namespace tmloader;
using namespace tmloader::hosted;

namespace
{

constexpr DWORD kReceiveTimeoutMs = 5000;

// Stands in for the system browser: the fake Supabase answers the authorize
// request with its redirect target in the body, which we then visit.
class ScriptedBrowser
{
public:
    auto opener() -> OpenBrowser
    {
        return [this](const std::string& url)
        {
            visits_.emplace_back([this, url]
                                 {
                                     try
                                     {
                                         const auto redirect = http_.send("GET", url);
                                         http_.send("GET", redirect.body);
                                     }
                                     catch (const std::exception&)
                                     {
                                     }
                                 });
        };
    }

private:
    HttpClient                http_{ true };
    std::vector<std::jthread> visits_;
};

auto connectLocal(unsigned short port) -> wil::unique_socket
{
    auto native             = wil::unique_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    auto address            = sockaddr_in{ AF_INET, htons(port) };
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(native.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    auto timeout = kReceiveTimeoutMs;
    setsockopt(native.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return native;
}

auto receiveSome(SOCKET native) -> int
{
    auto buffer = std::array<char, 4096>{};
    return recv(native, buffer.data(), static_cast<int>(buffer.size()), 0);
}

} // namespace

TEST_CASE("Hosted play through a live gateway", "[.gateway]")
{
    const auto* const origin = std::getenv("TMLOADER_GATEWAY_ORIGIN");
    REQUIRE(origin);
    auto config             = Config{};
    config.hostedServer     = origin;
    config.insecureLoopback = true;

    auto          browser = ScriptedBrowser{};
    HostedSession first(config);
    first.logout();
    REQUIRE_FALSE(first.resume());
    REQUIRE(first.signIn(browser.opener(), {}));
    REQUIRE(first.signedIn());

    first.startPlay();
    const auto game = first.gameConfig(config);
    REQUIRE(game.polId == "EEEEEEEF");
    REQUIRE(game.server == "127.0.0.1");
    // The game service speaks first, so bytes prove the tunnel reached it.
    const auto stream = connectLocal(game.gamePort);
    REQUIRE(receiveSome(stream.get()) > 0);

    // A second launcher resumes the remembered (rotated) login and meets the one-session rule.
    HostedSession second(config);
    REQUIRE(second.resume());
    try
    {
        second.startPlay();
        FAIL("a second play session was allowed");
    }
    catch (const ApiError& error)
    {
        REQUIRE(error.status() == 409);
        REQUIRE(error.code() == "session_conflict");
    }

    second.disconnectOtherSession();
    second.startPlay();
    // The first session's stream is revoked and reaches the game as a disconnect.
    auto result = int{};
    do
    {
        result = receiveSome(stream.get());
    } while (result > 0);
    REQUIRE(result == 0);

    second.logout();
    HostedSession third(config);
    REQUIRE_FALSE(third.resume());
}
