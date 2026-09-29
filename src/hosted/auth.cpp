#include <winsock2.h>

#include "app/log.h"
#include "config/config.h"
#include "hosted/auth.h"
#include <algorithm>
#include <bcrypt.h>
#include <format>
#include <nlohmann/json.hpp>
#include <wincred.h>
#include <wincrypt.h>

namespace tmloader::hosted
{
namespace
{

constexpr auto signInTimeout = std::chrono::minutes(5);

auto randomBytes(size_t count) -> std::vector<std::uint8_t>
{
    auto bytes = std::vector<std::uint8_t>(count);
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        throw std::runtime_error("Cannot generate random bytes");
    }

    return bytes;
}

auto json(const Response& response) -> nlohmann::json
{
    if (!response.contentType.starts_with("application/json"))
    {
        throw ApiError(response.status, "");
    }

    return nlohmann::json::parse(response.body);
}

auto errorFrom(const Response& response) -> ApiError
{
    try
    {
        const auto body = json(response);
        for (const auto* const key : { "error", "error_code" })
        {
            if (body.contains(key) && body[key].is_string())
            {
                return ApiError(response.status, body[key].get<std::string>());
            }
        }
    }
    catch (const std::exception&)
    {
    }

    return ApiError(response.status, "");
}

auto succeeded(Response response) -> Response
{
    if (response.status < 200 || response.status > 299)
    {
        throw errorFrom(response);
    }

    return response;
}

auto queryValue(std::string_view query, std::string_view name) -> std::optional<std::string>
{
    while (!query.empty())
    {
        const auto end   = query.find('&');
        const auto pair  = query.substr(0, end);
        const auto split = pair.find('=');
        if (pair.substr(0, split) == name && split != std::string_view::npos)
        {
            return percentDecode(pair.substr(split + 1));
        }

        if (end == std::string_view::npos)
        {
            break;
        }

        query.remove_prefix(end + 1);
    }

    return std::nullopt;
}

// Streams must stay on the configured server; its token is never sent elsewhere.
void requireSameOrigin(const std::string& stream, const std::string& origin)
{
    const auto prefix = [&]() -> std::string
    {
        if (origin.starts_with("https://"))
        {
            return "wss://" + origin.substr(8) + "/";
        }

        return "ws://" + origin.substr(7) + "/";
    }();
    if (!stream.starts_with(prefix))
    {
        throw std::runtime_error("Server returned a stream on another origin");
    }
}

constexpr auto pageStyle = "body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0c0e14;color:#e8e6e3;"
                           "font:17px 'Segoe UI',system-ui,sans-serif}main{text-align:center;padding:2rem 3rem;border-radius:12px;"
                           "background:#171a23;box-shadow:0 8px 32px #0008}h1{margin:0 0 .5rem;font-size:1.6rem;letter-spacing:.08em}"
                           "p{margin:.25rem 0;color:#a8a6a3}";

auto page(const char* status, const char* heading, const char* message) -> std::string
{
    // Tabs the page opened itself can close; otherwise the message stays.
    const auto body = std::format("<!doctype html><meta charset=utf-8><title>Tetra Master</title><style>{}</style>"
                                  "<main><h1>{}</h1><p>{}</p></main><script>setTimeout(()=>window.close(),1500)</script>",
                                  pageStyle,
                                  heading,
                                  message);
    return std::format("HTTP/1.1 {}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n{}", status, body.size(), body);
}

// Reads one HTTP request head from an accepted loopback connection.
auto readRequest(SOCKET client) -> std::string
{
    auto timeout = DWORD{ 2000 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    auto request = std::string{};
    auto chunk   = std::array<char, 2048>{};
    while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos)
    {
        const auto read = recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (read <= 0)
        {
            break;
        }

        request.append(chunk.data(), static_cast<size_t>(read));
    }

    return request;
}

void reply(SOCKET client, const std::string& response)
{
    send(client, response.data(), static_cast<int>(response.size()), 0);
    shutdown(client, SD_BOTH);
}

} // namespace

ApiError::ApiError(DWORD status, std::string code)
: std::runtime_error(std::format("HTTP {} {}", status, code))
, status_(status)
, code_(std::move(code))
{
}

auto ApiError::status() const noexcept -> DWORD
{
    return status_;
}

auto ApiError::code() const noexcept -> const std::string&
{
    return code_;
}

auto base64Url(std::span<const std::uint8_t> bytes) -> std::string
{
    constexpr auto flags  = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    auto           length = DWORD{};
    CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags, nullptr, &length);
    auto text = std::string(length, '\0');
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags, text.data(), &length))
    {
        throw std::runtime_error("Base64 encoding failed");
    }

    text.resize(length);
    std::erase(text, '=');
    std::ranges::replace(text, '+', '-');
    std::ranges::replace(text, '/', '_');
    return text;
}

auto makeVerifier() -> std::string
{
    return base64Url(randomBytes(32));
}

auto challengeFor(std::string_view verifier) -> std::string
{
    return base64Url(sha256(verifier.data(), verifier.size()));
}

auto parseCallback(std::string_view request, std::string_view path) -> Callback
{
    const auto lineEnd = request.find("\r\n");
    const auto line    = request.substr(0, lineEnd);
    if (lineEnd == std::string_view::npos || !line.starts_with("GET ") || !line.ends_with(" HTTP/1.1"))
    {
        return {};
    }

    const auto target = line.substr(4, line.size() - 4 - 9);
    const auto split  = target.find('?');
    if (target.substr(0, split) != path || split == std::string_view::npos)
    {
        return {};
    }

    const auto query = target.substr(split + 1);
    if (const auto code = queryValue(query, "code"); code && !code->empty())
    {
        return { Callback::Kind::code, *code };
    }

    const auto error = queryValue(query, "error_description").value_or(queryValue(query, "error").value_or("sign-in failed"));
    return { Callback::Kind::error, error };
}

auto parseAuthSettings(const Response& response) -> AuthSettings
{
    const auto body = json(succeeded(response));
    if (body.at("protocol") != 1 || body.at("provider") != "discord")
    {
        throw std::runtime_error("Unsupported hosted server protocol or sign-in provider");
    }

    auto settings = AuthSettings{ body.at("supabase_url").get<std::string>(), body.at("publishable_key").get<std::string>(), body.at("stream_subprotocol").get<std::string>() };
    if (!isAllowedOrigin(settings.supabaseUrl, true) || settings.publishableKey.empty() || settings.subprotocol != "tm-stream-v1")
    {
        throw std::runtime_error("Hosted server returned an invalid sign-in configuration");
    }

    return settings;
}

auto parseTokens(const Response& response) -> Tokens
{
    const auto body   = json(succeeded(response));
    auto       tokens = Tokens{ body.at("access_token").get<std::string>(), body.at("refresh_token").get<std::string>() };
    tokens.expiresAt  = std::chrono::system_clock::now() + std::chrono::seconds(body.at("expires_in").get<int>());
    const auto text   = [](const nlohmann::json& object, const char* key) -> std::string
    {
        if (object.is_object() && object.contains(key) && object[key].is_string())
        {
            return object[key].get<std::string>();
        }

        return {};
    };
    if (body.contains("user"))
    {
        const auto& user    = body["user"];
        tokens.userId       = text(user, "id");
        const auto metadata = user.value("user_metadata", nlohmann::json::object());
        // Discord's global display name, falling back to the account name.
        for (const auto& name : { text(metadata.value("custom_claims", nlohmann::json::object()), "global_name"), text(metadata, "full_name"), text(metadata, "name") })
        {
            if (tokens.displayName.empty())
            {
                tokens.displayName = name;
            }
        }

        tokens.avatarUrl = text(metadata, "avatar_url");
    }

    if (tokens.access.empty() || tokens.refresh.empty())
    {
        throw std::runtime_error("Sign-in returned no tokens");
    }

    return tokens;
}

auto parsePlaySession(const Response& response) -> PlaySession
{
    const auto body = json(succeeded(response));
    if (body.at("protocol") != 1)
    {
        throw std::runtime_error("Unsupported play-session protocol");
    }

    const auto& legacy  = body.at("legacy");
    const auto& streams = body.at("streams");
    return PlaySession{
        body.at("session_token").get<std::string>(),
        std::chrono::seconds(body.at("expires_in").get<int>()),
        legacy.at("pol_id").get<std::string>(),
        legacy.at("password").get<std::string>(),
        streams.at("game").get<std::string>(),
        streams.at("profile").get<std::string>(),
    };
}

AuthClient::AuthClient(HttpClient& http, std::string origin)
: http_(http)
, origin_(std::move(origin))
{
}

auto AuthClient::settings() -> const AuthSettings&
{
    if (!settings_)
    {
        settings_ = parseAuthSettings(http_.send("GET", origin_ + "/v1/auth/config"));
    }

    return *settings_;
}

auto AuthClient::signIn(const OpenBrowser& openBrowser, std::stop_token stop) -> std::optional<Tokens>
{
    const auto& auth        = settings();
    const auto  listener    = wil::unique_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    auto        address     = sockaddr_in{ AF_INET };
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto length             = int{ sizeof(address) };
    if (!listener || bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener.get(), 4) ||
        getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length))
    {
        throw std::runtime_error("Cannot open the sign-in callback listener");
    }

    const auto verifier = makeVerifier();
    const auto path     = "/callback/" + base64Url(randomBytes(16));
    const auto redirect = std::format("http://127.0.0.1:{}{}", ntohs(address.sin_port), path);
    openBrowser(std::format("{}/auth/v1/authorize?provider=discord&redirect_to={}&code_challenge={}&code_challenge_method=s256",
                            auth.supabaseUrl,
                            percentEncode(redirect),
                            percentEncode(challengeFor(verifier))));

    const auto deadline = std::chrono::steady_clock::now() + signInTimeout;
    while (!stop.stop_requested())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw std::runtime_error("Sign-in timed out");
        }

        auto ready = fd_set{};
        FD_SET(listener.get(), &ready);
        const auto wait = timeval{ 0, 250000 };
        if (select(0, &ready, nullptr, nullptr, &wait) <= 0)
        {
            continue;
        }

        const auto client   = wil::unique_socket(accept(listener.get(), nullptr, nullptr));
        const auto callback = parseCallback(readRequest(client.get()), path);
        if (callback.kind == Callback::Kind::other)
        {
            reply(client.get(), page("404 Not Found", "TETRA MASTER", "This sign-in link is no longer valid."));
            continue;
        }

        if (callback.kind == Callback::Kind::error)
        {
            reply(client.get(), page("400 Bad Request", "Sign-in cancelled", "Return to Tetra Master to try again. You can close this tab."));
            throw std::runtime_error("Sign-in was cancelled: " + callback.value);
        }

        reply(client.get(), page("200 OK", "You're signed in", "Returning to Tetra Master. You can close this tab."));
        const auto body = nlohmann::json{ { "auth_code", callback.value }, { "code_verifier", verifier } }.dump();
        return parseTokens(http_.send("POST", auth.supabaseUrl + "/auth/v1/token?grant_type=pkce", { { "apikey", auth.publishableKey }, { "Content-Type", "application/json" } }, body));
    }

    return std::nullopt;
}

auto AuthClient::refresh(const std::string& refreshToken) -> Tokens
{
    const auto& auth = settings();
    const auto  body = nlohmann::json{ { "refresh_token", refreshToken } }.dump();
    return parseTokens(http_.send("POST", auth.supabaseUrl + "/auth/v1/token?grant_type=refresh_token", { { "apikey", auth.publishableKey }, { "Content-Type", "application/json" } }, body));
}

auto AuthClient::call(const char* method, const std::string& path, const std::string& bearer, const std::string& body) -> Response
{
    auto headers = Headers{};
    if (!bearer.empty())
    {
        headers.emplace_back("Authorization", "Bearer " + bearer);
    }

    if (!body.empty())
    {
        headers.emplace_back("Content-Type", "application/json");
    }

    return succeeded(http_.send(method, origin_ + path, headers, body));
}

auto AuthClient::issueTicket(const std::string& access) -> std::string
{
    return json(call("POST", "/v1/play/tickets", access, R"({"protocol":1})")).at("ticket").get<std::string>();
}

auto AuthClient::redeem(const std::string& ticket) -> PlaySession
{
    auto session = parsePlaySession(call("POST", "/v1/play/sessions", "", nlohmann::json{ { "ticket", ticket } }.dump()));
    requireSameOrigin(session.gameStream, origin_);
    requireSameOrigin(session.profileStream, origin_);
    return session;
}

void AuthClient::renew(const std::string& access, const std::string& sessionToken)
{
    call("POST", "/v1/play/renew", access, nlohmann::json{ { "session_token", sessionToken } }.dump());
}

void AuthClient::revokeSession(const std::string& sessionToken)
{
    call("DELETE", "/v1/play/session", sessionToken, "");
}

void AuthClient::revokeAccountSession(const std::string& access)
{
    call("DELETE", "/v1/play/account-session", access, "");
}

CredentialStore::CredentialStore(std::string_view origin, std::string_view supabaseUrl)
{
    const auto target = std::format("tmloader:{}|{}", origin, supabaseUrl);
    target_           = std::wstring(target.begin(), target.end());
}

auto CredentialStore::load() const -> std::optional<std::string>
{
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &credential))
    {
        return std::nullopt;
    }

    const auto owned = wil::unique_any<PCREDENTIALW, decltype(&CredFree), CredFree>(credential);
    return std::string(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
}

void CredentialStore::save(const std::string& refreshToken) const
{
    auto credential               = CREDENTIALW{};
    credential.Type               = CRED_TYPE_GENERIC;
    credential.TargetName         = const_cast<wchar_t*>(target_.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(refreshToken.size());
    credential.CredentialBlob     = reinterpret_cast<BYTE*>(const_cast<char*>(refreshToken.data()));
    credential.Persist            = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName           = const_cast<wchar_t*>(L"tmloader");
    if (!CredWriteW(&credential, 0))
    {
        logLine("Could not remember sign-in (error {})", GetLastError());
    }
}

void CredentialStore::erase() const noexcept
{
    CredDeleteW(target_.c_str(), CRED_TYPE_GENERIC, 0);
}

} // namespace tmloader::hosted
