#include <winsock2.h>
#include <ws2tcpip.h>

#include "login/login.h"
#include "login/wire.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <wil/resource.h>

using namespace tmloader;

namespace tmloader
{

void writeLog(std::string_view)
{
}

} // namespace tmloader

namespace
{

constexpr std::array<std::uint8_t, 8> kSessionKey{ 1, 2, 3, 4, 5, 6, 7, 8 };

struct Winsock
{
    Winsock()
    {
        auto data = WSADATA{};
        REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }

    ~Winsock()
    {
        WSACleanup();
    }

    Winsock(const Winsock&)                    = delete;
    auto operator=(const Winsock&) -> Winsock& = delete;
};

auto readLine(SOCKET socket) -> std::string
{
    auto line = std::string{};
    char byte = 0;
    while (recv(socket, &byte, 1, 0) == 1)
    {
        line += byte;
        if (byte == '\n')
        {
            break;
        }
    }

    return line;
}

void sendAll(SOCKET socket, const std::string& data)
{
    send(socket, data.data(), static_cast<int>(data.size()), 0);
}

// A one-connection game port that runs the key exchange like tmserver and
// answers LOGIN with reply. received holds the decrypted LOGIN line.
class FakeServer
{
public:
    explicit FakeServer(std::string reply, bool closeAfterNotice = false)
    : listener_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
    {
        auto address            = sockaddr_in{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(listen(listener_.get(), 1) == 0);
        auto length = static_cast<int>(sizeof(address));
        getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
        port_   = ntohs(address.sin_port);
        thread_ = std::jthread([this, reply = std::move(reply), closeAfterNotice]
                               {
                                   serve(reply, closeAfterNotice);
                               });
    }

    auto port() const -> unsigned short
    {
        return port_;
    }

    auto received() -> std::string
    {
        thread_.join();
        return received_;
    }

private:
    void serve(const std::string& reply, bool closeAfterNotice)
    {
        const auto peer = wil::unique_socket(accept(listener_.get(), nullptr, nullptr));
        sendAll(peer.get(), wire::frameLine(":srv NOTICE AUTH CHALLENGE", nullptr, 0));
        if (closeAfterNotice)
        {
            return;
        }

        const auto user    = wire::unframeLine(readLine(peer.get()), nullptr, 0);
        const auto decoded = wire::decodeBase64(user.substr(user.rfind(':') + 1));
        auto       modulus = std::array<std::uint8_t, 32>{};
        std::memcpy(modulus.data(), decoded.data(), modulus.size());
        auto block = std::array<std::uint8_t, 32>{};
        block[1]   = 2;
        std::fill(block.begin() + 2, block.begin() + 23, std::uint8_t{ 0x33 });
        std::reverse_copy(kSessionKey.begin(), kSessionKey.end(), block.begin() + 24);
        sendAll(peer.get(), wire::frameLine(":srv 300 * " + wire::encodeBase64(wire::rsaEncrypt(modulus, block), true), nullptr, 0));

        const auto cipher = wire::Blowfish(kSessionKey);
        auto       iv     = std::uint64_t{};
        std::memcpy(&iv, modulus.data(), sizeof(iv));
        received_ = wire::unframeLine(readLine(peer.get()), &cipher, iv);
        sendAll(peer.get(), wire::frameLine(reply, &cipher, iv));
    }

    wil::unique_socket listener_;
    unsigned short     port_ = 0;
    std::string        received_;
    std::jthread       thread_;
};

} // namespace

TEST_CASE("LOGIN creates an account and returns the game password", "[login]")
{
    const auto winsock = Winsock{};
    auto       server  = FakeServer(":srv 900 ABCDEFGH 0123456789abcd :created");
    const auto result  = login("127.0.0.1", server.port(), "ABCDEFGH", "pass word=1");
    REQUIRE(server.received() == "LOGIN ABCDEFGH :pass word=1");
    REQUIRE(result.polId == "ABCDEFGH");
    REQUIRE(result.gamePassword == "0123456789abcd");
    REQUIRE(result.created);
}

TEST_CASE("LOGIN signs in to an existing account", "[login]")
{
    const auto winsock = Winsock{};
    auto       server  = FakeServer(":srv 900 ABCDEFGH fedcba98765432 :signed_in");
    const auto result  = login("127.0.0.1", server.port(), "ABCDEFGH", "secret");
    REQUIRE_FALSE(result.created);
    REQUIRE(result.gamePassword == "fedcba98765432");
}

TEST_CASE("a refused LOGIN reports the server's code", "[login]")
{
    const auto winsock = Winsock{};
    auto       server  = FakeServer(":srv 901 invalid_credentials :wrong password");
    try
    {
        login("127.0.0.1", server.port(), "ABCDEFGH", "secret");
        FAIL("LOGIN succeeded");
    }
    catch (const LoginError& error)
    {
        REQUIRE(error.code() == "invalid_credentials");
        REQUIRE(describeLoginError(error) == "Wrong password for this POL ID.");
    }
}

TEST_CASE("a reply for another account or a malformed game password is refused", "[login]")
{
    const auto winsock = Winsock{};
    for (const auto* reply : { ":srv 900 OTHERPOL 0123456789abcd :created", ":srv 900 ABCDEFGH short :created", ":srv 900 ABCDEFGH 0123456789abcd :maybe" })
    {
        CAPTURE(reply);
        auto server = FakeServer(reply);
        REQUIRE_THROWS_AS(login("127.0.0.1", server.port(), "ABCDEFGH", "secret"), LoginError);
    }
}

TEST_CASE("a dropped connection is a transport error without a code", "[login]")
{
    const auto winsock = Winsock{};
    auto       server  = FakeServer({}, true);
    try
    {
        login("127.0.0.1", server.port(), "ABCDEFGH", "secret", std::chrono::seconds(5));
        FAIL("LOGIN succeeded");
    }
    catch (const LoginError& error)
    {
        REQUIRE(error.code().empty());
        REQUIRE(describeLoginError(error) == error.what());
    }
}

TEST_CASE("every server error code has its own message", "[login]")
{
    auto messages = std::set<std::string>{};
    for (const auto* code : { "invalid_request", "invalid_credentials", "pol_id_unavailable", "account_locked", "account_connected", "rate_limited", "unavailable" })
    {
        messages.insert(describeLoginError(LoginError(code, "text")));
    }

    REQUIRE(messages.size() == 7);
    REQUIRE(describeLoginError(LoginError("new_code", "details")).contains("new_code"));
}

TEST_CASE("numeric addresses resolve to themselves", "[login]")
{
    REQUIRE(resolveIpv4("127.0.0.1") == "127.0.0.1");
    REQUIRE(resolveIpv4("localhost") == "127.0.0.1");
    REQUIRE_THROWS_AS(resolveIpv4("no-such-host.invalid"), LoginError);
}

// Hidden: logs in to a real server, e.g. tmserver started for a manual check.
// Set TMLOADER_LOGIN_SERVER=host:port, TMLOADER_LOGIN_POL_ID and TMLOADER_LOGIN_PASSWORD.
TEST_CASE("LOGIN against a live server", "[.live]")
{
    const auto read = [](const char* name) -> std::string
    {
        auto value = std::array<char, 256>{};
        if (GetEnvironmentVariableA(name, value.data(), static_cast<DWORD>(value.size())) == 0)
        {
            return {};
        }

        return value.data();
    };
    const auto server = read("TMLOADER_LOGIN_SERVER");
    REQUIRE(server.contains(':'));
    const auto host    = server.substr(0, server.rfind(':'));
    const auto port    = static_cast<unsigned short>(std::stoi(server.substr(server.rfind(':') + 1)));
    const auto result  = login(resolveIpv4(host), port, read("TMLOADER_LOGIN_POL_ID"), read("TMLOADER_LOGIN_PASSWORD"));
    auto       outcome = "signed_in";
    if (result.created)
    {
        outcome = "created";
    }

    std::printf("%s %s %s\n", result.polId.c_str(), result.gamePassword.c_str(), outcome);
    REQUIRE(result.gamePassword.size() == 14);
}
