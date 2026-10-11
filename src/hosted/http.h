#pragma once
#include <winsock2.h>

#include <windows.h>

#include <winhttp.h>

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <wil/resource.h>

namespace tmloader::hosted
{

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Response
{
    DWORD       status = 0;
    std::string contentType;
    std::string body;
};

// A transport failure, as opposed to an HTTP error status.
class TransportError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// One WebSocket carrying binary messages; send and receive may run on different threads.
class WebSocket
{
public:
    explicit WebSocket(wil::unique_winhttp_hinternet socket);

    void send(std::span<const char> data);
    // A complete binary message, or nothing once the peer closed. Throws on text or oversized messages.
    auto receive() -> std::optional<std::vector<char>>;
    void close(USHORT code) noexcept;
    auto closeStatus() const -> USHORT;

private:
    wil::unique_winhttp_hinternet socket_;
};

// HTTPS client that never follows redirects. Plain HTTP is accepted only for
// 127.0.0.1 and only when insecure loopback is enabled.
class HttpClient
{
public:
    explicit HttpClient(bool insecureLoopback);

    auto send(const char* method, const std::string& url, const Headers& headers = {}, const std::string& body = {}) -> Response;
    auto openWebSocket(const std::string& url, const std::string& bearer, const std::string& subprotocol) -> WebSocket;

private:
    auto open(const char* method, const std::string& url, const Headers& headers) -> std::pair<wil::unique_winhttp_hinternet, wil::unique_winhttp_hinternet>;

    bool                          insecureLoopback_;
    wil::unique_winhttp_hinternet session_;
};

// Percent-encodes everything except RFC 3986 unreserved characters.
auto percentEncode(std::string_view text) -> std::string;

} // namespace tmloader::hosted
