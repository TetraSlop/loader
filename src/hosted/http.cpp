#include "hosted/http.h"
#include "app/log.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace tmloader::hosted
{
namespace
{

constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr size_t kMaxMessageBytes  = 65536;

[[noreturn]] void fail(const char* action)
{
    throw TransportError(std::format("{} failed (error {})", action, GetLastError()));
}

auto queryHeader(HINTERNET request, DWORD query, const wchar_t* name = WINHTTP_HEADER_NAME_BY_INDEX) -> std::optional<std::string>
{
    auto size = DWORD{};
    WinHttpQueryHeaders(request, query, name, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return std::nullopt;
    }

    auto value = std::wstring(size / sizeof(wchar_t), wchar_t{});
    if (!WinHttpQueryHeaders(request, query, name, value.data(), &size, WINHTTP_NO_HEADER_INDEX))
    {
        return std::nullopt;
    }

    value.resize(size / sizeof(wchar_t));
    return narrow(value);
}

auto statusCode(HINTERNET request) -> DWORD
{
    auto status = DWORD{};
    auto size   = DWORD{ sizeof(status) };
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
    {
        fail("Reading HTTP status");
    }

    return status;
}

} // namespace

WebSocket::WebSocket(wil::unique_winhttp_hinternet socket)
: socket_(std::move(socket))
{
}

void WebSocket::send(std::span<const char> data)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    const auto result = WinHttpWebSocketSend(socket_.get(), WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<char*>(data.data()), static_cast<DWORD>(data.size()));
    if (result != ERROR_SUCCESS)
    {
        throw TransportError(std::format("WebSocket send failed (error {})", result));
    }
}

auto WebSocket::receive() -> std::optional<std::vector<char>>
{
    auto message = std::vector<char>(kMaxMessageBytes + 1);
    auto length  = size_t{};
    while (true)
    {
        auto       read   = DWORD{};
        auto       type   = WINHTTP_WEB_SOCKET_BUFFER_TYPE{};
        const auto result = WinHttpWebSocketReceive(socket_.get(), message.data() + length, static_cast<DWORD>(message.size() - length), &read, &type);
        // Our own close() cancels a pending receive; that is a normal end of stream.
        if (result == ERROR_WINHTTP_OPERATION_CANCELLED)
        {
            return std::nullopt;
        }

        if (result != ERROR_SUCCESS)
        {
            throw TransportError(std::format("WebSocket receive failed (error {})", result));
        }

        length += read;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
        {
            return std::nullopt;
        }

        if (type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
        {
            throw TransportError("WebSocket sent a text message");
        }

        if (length > kMaxMessageBytes)
        {
            throw TransportError("WebSocket message exceeds 64 KiB");
        }

        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        {
            message.resize(length);
            return message;
        }
    }
}

void WebSocket::close(USHORT code) noexcept
{
    WinHttpWebSocketClose(socket_.get(), code, nullptr, 0);
}

auto WebSocket::closeStatus() const -> USHORT
{
    auto status   = USHORT{};
    auto received = DWORD{};
    if (WinHttpWebSocketQueryCloseStatus(socket_.get(), &status, nullptr, 0, &received) != ERROR_SUCCESS)
    {
        return 0;
    }

    return status;
}

HttpClient::HttpClient(bool insecureLoopback)
: insecureLoopback_(insecureLoopback)
, session_(WinHttpOpen(L"tmloader/" TMLOADER_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
{
    if (!session_)
    {
        fail("Opening WinHTTP");
    }

    auto protocols = DWORD{ WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 };
    WinHttpSetOption(session_.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session_.get(), 10000, 10000, 15000, 15000);
}

auto HttpClient::open(const char* method, const std::string& url, const Headers& headers) -> std::pair<wil::unique_winhttp_hinternet, wil::unique_winhttp_hinternet>
{
    const auto wideUrl      = widen(url);
    auto       parts        = URL_COMPONENTS{ sizeof(URL_COMPONENTS) };
    parts.dwHostNameLength  = static_cast<DWORD>(-1);
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) || parts.dwUserNameLength || parts.dwPasswordLength)
    {
        throw TransportError("Invalid URL");
    }

    const auto host   = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
    const auto secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(insecureLoopback_ && parts.nScheme == INTERNET_SCHEME_HTTP && host == L"127.0.0.1"))
    {
        throw TransportError("Refusing a non-HTTPS URL");
    }

    auto connection = wil::unique_winhttp_hinternet(WinHttpConnect(session_.get(), host.c_str(), parts.nPort, 0));
    if (!connection)
    {
        fail("Connecting");
    }

    const auto target = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    auto       flags  = DWORD{};
    if (secure)
    {
        flags = WINHTTP_FLAG_SECURE;
    }

    auto request = wil::unique_winhttp_hinternet(WinHttpOpenRequest(connection.get(), widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request)
    {
        fail("Opening request");
    }

    auto disabled = DWORD{ WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION };
    WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    for (const auto& [name, value] : headers)
    {
        const auto line = widen(std::format("{}: {}", name, value));
        if (!WinHttpAddRequestHeaders(request.get(), line.c_str(), static_cast<DWORD>(line.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        {
            fail("Adding request header");
        }
    }

    return { std::move(connection), std::move(request) };
}

auto HttpClient::send(const char* method, const std::string& url, const Headers& headers, const std::string& body) -> Response
{
    const auto [connection, request] = open(method, url, headers);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* const data = const_cast<char*>(body.data());
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
    {
        fail("HTTP request");
    }

    auto response        = Response{};
    response.status      = statusCode(request.get());
    response.contentType = queryHeader(request.get(), WINHTTP_QUERY_CONTENT_TYPE).value_or("");
    auto chunk           = std::array<char, 16384>{};
    auto read            = DWORD{};
    while (true)
    {
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read))
        {
            fail("Reading HTTP response");
        }

        if (!read)
        {
            break;
        }

        if (response.body.size() + read > kMaxResponseBytes)
        {
            throw TransportError("HTTP response too large");
        }

        response.body.append(chunk.data(), read);
    }

    return response;
}

auto HttpClient::openWebSocket(const std::string& url, const std::string& bearer, const std::string& subprotocol) -> WebSocket
{
    const auto headers = Headers{ { "Authorization", "Bearer " + bearer }, { "Sec-WebSocket-Protocol", subprotocol } };
    // WinHTTP upgrades an http(s) request; open() applies the same scheme rules.
    const auto httpUrl = [&]() -> std::string
    {
        if (url.starts_with("wss://"))
        {
            return "https://" + url.substr(6);
        }

        if (url.starts_with("ws://"))
        {
            return "http://" + url.substr(5);
        }

        throw TransportError("Stream URL must use ws:// or wss://");
    }();
    const auto [connection, request] = open("GET", httpUrl, headers);
    WinHttpSetTimeouts(request.get(), 10000, 10000, 15000, 0); // Streams may idle.
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
    {
        fail("WebSocket upgrade");
    }

    if (const auto status = statusCode(request.get()); status != HTTP_STATUS_SWITCH_PROTOCOLS)
    {
        throw TransportError(std::format("WebSocket upgrade refused (HTTP {})", status));
    }

    if (queryHeader(request.get(), WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol") != subprotocol)
    {
        throw TransportError("Server did not select the stream subprotocol");
    }

    auto socket = wil::unique_winhttp_hinternet(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
    if (!socket)
    {
        fail("WebSocket upgrade");
    }

    return WebSocket(std::move(socket));
}

auto percentEncode(std::string_view text) -> std::string
{
    auto result = std::string{};
    for (const auto c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~')
        {
            result += c;
        }
        else
        {
            result += std::format("%{:02X}", byte);
        }
    }

    return result;
}

auto percentDecode(std::string_view text) -> std::string
{
    auto result = std::string{};
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2])))
        {
            result += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        }
        else if (text[i] == '+')
        {
            result += ' ';
        }
        else
        {
            result += text[i];
        }
    }

    return result;
}

} // namespace tmloader::hosted
