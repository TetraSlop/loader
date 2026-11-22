#include <winsock2.h>
#include <ws2tcpip.h>

#include "login/login.h"
#include "login/wire.h"
#include <array>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>
#include <wil/resource.h>

namespace tmloader
{
namespace
{

// tmserver's line limit; anything longer is not a reply we expect.
constexpr size_t kMaxLine            = 1024;
constexpr size_t kGamePasswordLength = 14;

class Winsock
{
public:
    Winsock()
    {
        auto data = WSADATA{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            throw LoginError({}, "Cannot start Windows networking");
        }
    }

    ~Winsock()
    {
        WSACleanup();
    }

    Winsock(const Winsock&)                    = delete;
    auto operator=(const Winsock&) -> Winsock& = delete;
};

auto connectTo(const std::string& address, unsigned short port, std::chrono::milliseconds timeout) -> wil::unique_socket
{
    auto target       = sockaddr_in{};
    target.sin_family = AF_INET;
    target.sin_port   = htons(port);
    if (InetPtonA(AF_INET, address.c_str(), &target.sin_addr) != 1)
    {
        throw LoginError({}, "Invalid server address " + address);
    }

    auto socket = wil::unique_socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket)
    {
        throw LoginError({}, "Cannot create a socket");
    }

    // Connect without blocking so an unreachable server fails within the timeout.
    auto nonBlocking = u_long{ 1 };
    ioctlsocket(socket.get(), FIONBIO, &nonBlocking);
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        throw LoginError({}, std::format("Cannot connect to {}:{}", address, port));
    }

    auto writable = fd_set{};
    auto failed   = fd_set{};
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket.get(), &writable);
    FD_SET(socket.get(), &failed);
    const auto wait = timeval{ static_cast<long>(timeout.count() / 1000), static_cast<long>(timeout.count() % 1000 * 1000) };
    if (select(0, nullptr, &writable, &failed, &wait) != 1 || FD_ISSET(socket.get(), &failed))
    {
        throw LoginError({}, std::format("Cannot connect to {}:{}", address, port));
    }

    nonBlocking = 0;
    ioctlsocket(socket.get(), FIONBIO, &nonBlocking);
    const auto milliseconds = static_cast<DWORD>(timeout.count());
    setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
    setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
    return socket;
}

class Connection
{
public:
    explicit Connection(wil::unique_socket socket)
    : socket_(std::move(socket))
    {
    }

    void send(std::string_view data)
    {
        while (!data.empty())
        {
            const auto sent = ::send(socket_.get(), data.data(), static_cast<int>(data.size()), 0);
            if (sent <= 0)
            {
                throw LoginError({}, "The connection to the server was lost");
            }

            data.remove_prefix(static_cast<size_t>(sent));
        }
    }

    // One line including its CRLF.
    auto readLine() -> std::string
    {
        for (;;)
        {
            if (const auto end = buffer_.find('\n'); end != std::string::npos)
            {
                auto line = buffer_.substr(0, end + 1);
                buffer_.erase(0, end + 1);
                return line;
            }

            if (buffer_.size() > kMaxLine)
            {
                throw LoginError({}, "The server sent an oversized line");
            }

            auto       chunk    = std::array<char, 512>{};
            const auto received = recv(socket_.get(), chunk.data(), static_cast<int>(chunk.size()), 0);
            if (received == 0)
            {
                throw LoginError({}, "The server closed the connection");
            }

            if (received < 0)
            {
                if (WSAGetLastError() == WSAETIMEDOUT)
                {
                    throw LoginError({}, "The server did not answer in time");
                }

                throw LoginError({}, "The connection to the server was lost");
            }

            buffer_.append(chunk.data(), static_cast<size_t>(received));
        }
    }

private:
    wil::unique_socket socket_;
    std::string        buffer_;
};

// Splits ":srv 900 A B :trailing" into its space-separated words and the trailing text.
auto splitReply(const std::string& message) -> std::pair<std::vector<std::string>, std::string>
{
    auto head     = message;
    auto trailing = std::string{};
    if (const auto colon = message.find(" :"); colon != std::string::npos)
    {
        head     = message.substr(0, colon);
        trailing = message.substr(colon + 2);
    }

    auto words = std::vector<std::string>{};
    for (size_t start = 0; start < head.size();)
    {
        const auto end = std::min(head.find(' ', start), head.size());
        if (end > start)
        {
            words.push_back(head.substr(start, end - start));
        }

        start = end + 1;
    }

    return { words, trailing };
}

[[noreturn]] void refused(const std::vector<std::string>& words, const std::string& trailing)
{
    auto code = std::string("unknown");
    if (words.size() >= 3)
    {
        code = words[2];
    }

    if (trailing.empty())
    {
        throw LoginError(code, code);
    }

    throw LoginError(code, trailing);
}

auto isHex(const std::string& text) -> bool
{
    return text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

} // namespace

LoginError::LoginError(std::string code, const std::string& message)
: std::runtime_error(message)
, code_(std::move(code))
{
}

auto LoginError::code() const -> const std::string&
{
    return code_;
}

auto resolveIpv4(const std::string& host) -> std::string
{
    const auto winsock = Winsock{};
    auto       hints   = addrinfo{};
    hints.ai_family    = AF_INET;
    hints.ai_socktype  = SOCK_STREAM;
    addrinfo* found    = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found)
    {
        throw LoginError({}, "Cannot find the server " + host);
    }

    const auto results = wil::unique_addrinfo_ansi(found);
    char       text[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, &reinterpret_cast<const sockaddr_in*>(results.get()->ai_addr)->sin_addr, text, sizeof(text));
    return text;
}

auto login(const std::string& server, unsigned short port, const std::string& polId, const std::string& password, std::chrono::milliseconds timeout) -> LoginResult
{
    const auto winsock    = Winsock{};
    auto       connection = Connection(connectTo(server, port, timeout));

    // The plaintext challenge comes first; LOGIN does not use it.
    if (!wire::unframeLine(connection.readLine(), nullptr, 0).contains(" NOTICE "))
    {
        throw LoginError({}, "The server did not start the key exchange");
    }

    const auto key = wire::generateRsaKey();
    connection.send(wire::frameLine("USER x 8 * :" + wire::encodeBase64(key.modulus, false), nullptr, 0));
    const auto [exchange, exchangeText] = splitReply(wire::unframeLine(connection.readLine(), nullptr, 0));
    if (exchange.size() >= 2 && exchange[1] == "901")
    {
        refused(exchange, exchangeText);
    }

    if (exchange.size() != 4 || exchange[1] != "300")
    {
        throw LoginError({}, "The server did not complete the key exchange");
    }

    const auto wrapped = wire::decodeBase64(exchange[3]);
    if (wrapped.size() < 32)
    {
        throw LoginError({}, "The server's key exchange is too short");
    }

    const auto block  = wire::rsaDecrypt(key, std::span<const std::uint8_t, 32>(wrapped.data(), 32));
    const auto cipher = wire::Blowfish(wire::sessionKey(block));
    auto       iv     = std::uint64_t{};
    std::memcpy(&iv, key.modulus.data(), sizeof(iv));

    connection.send(wire::frameLine("LOGIN " + polId + " :" + password, &cipher, iv));
    const auto [words, trailing] = splitReply(wire::unframeLine(connection.readLine(), &cipher, iv));
    if (words.size() >= 2 && words[1] == "901")
    {
        refused(words, trailing);
    }

    if (words.size() != 4 || words[1] != "900" || words[2] != polId || words[3].size() != kGamePasswordLength || !isHex(words[3]) ||
        (trailing != "created" && trailing != "signed_in"))
    {
        throw LoginError({}, "Unexpected reply from the server");
    }

    return { words[2], words[3], trailing == "created" };
}

auto describeLoginError(const LoginError& error) -> std::string
{
    const auto& code = error.code();
    if (code.empty())
    {
        return error.what();
    }

    if (code == "invalid_request")
    {
        return "The server rejected the login. Check that the POL ID and password are in the right format.";
    }

    if (code == "invalid_credentials")
    {
        return "Wrong password for this POL ID.";
    }

    if (code == "pol_id_unavailable")
    {
        return "This POL ID is reserved. Choose another one.";
    }

    if (code == "account_locked")
    {
        return "This account is locked. Contact the server operator.";
    }

    if (code == "account_connected")
    {
        return "This account is already playing. If you just left a game, try again in a couple of minutes.";
    }

    if (code == "rate_limited")
    {
        return "Too many attempts. Wait a while, then try again.";
    }

    if (code == "unavailable")
    {
        return "The server cannot sign you in right now. Try again shortly.";
    }

    return std::format("The server refused the login ({}): {}", code, error.what());
}

} // namespace tmloader
