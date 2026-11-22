#include <httplib.h>

#include "app/log.h"
#include "config/config.h"
#include "hosted/auth.h"
#include <algorithm>
#include <bcrypt.h>
#include <condition_variable>
#include <format>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <wincred.h>
#include <wincrypt.h>

namespace tmloader::hosted
{
namespace
{

constexpr auto kSignInTimeout = std::chrono::minutes(5);
// A connection that sends no complete request is dropped after this long.
constexpr auto kCallbackReadTimeout = std::chrono::seconds(2);
constexpr auto kBase64Flags         = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;

// Shared by the pages the loopback listener serves after sign-in.
constexpr auto kPageStyle = "body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0c0e14;color:#e8e6e3;"
                            "font:17px 'Segoe UI',system-ui,sans-serif}main{text-align:center;padding:2rem 3rem;border-radius:12px;"
                            "background:#171a23;box-shadow:0 8px 32px #0008}h1{margin:0 0 .5rem;font-size:1.6rem;letter-spacing:.08em}"
                            "p{margin:.25rem 0;color:#a8a6a3}";

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

// Streams must stay on the configured server; its token is never sent elsewhere.
void requireSameOrigin(const std::string& stream, const std::string& origin)
{
    auto prefix = "ws://" + origin.substr(7) + "/";
    if (origin.starts_with("https://"))
    {
        prefix = "wss://" + origin.substr(8) + "/";
    }

    if (!stream.starts_with(prefix))
    {
        throw std::runtime_error("Server returned a stream on another origin");
    }
}

void servePage(httplib::Response& response, httplib::StatusCode status, const char* heading, const char* message)
{
    // Tabs the page opened itself can close; otherwise the message stays.
    response.status = status;
    response.set_content(std::format("<!doctype html><meta charset=utf-8><title>Tetra Master</title><style>{}</style>"
                                     "<main><h1>{}</h1><p>{}</p></main><script>setTimeout(()=>window.close(),1500)</script>",
                                     kPageStyle,
                                     heading,
                                     message),
                         "text/html; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}

// What a request on the pending callback path carries. The GET route also receives HEAD.
auto callbackFrom(const httplib::Request& request) -> Callback
{
    if (request.method != "GET" || !request.target.contains('?'))
    {
        return {};
    }

    if (auto code = request.get_param_value("code"); !code.empty())
    {
        return { Callback::Kind::code, std::move(code) };
    }

    for (const auto* const key : { "error_description", "error" })
    {
        if (request.has_param(key))
        {
            return { Callback::Kind::error, request.get_param_value(key) };
        }
    }

    return { Callback::Kind::error, "sign-in failed" };
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
    auto length = DWORD{};
    CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), kBase64Flags, nullptr, &length);
    auto text = std::string(length, '\0');
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), kBase64Flags, text.data(), &length))
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

auto awaitCallback(const std::string& path, const std::function<void(unsigned short port)>& listening, const std::stop_token& stop, std::chrono::steady_clock::duration timeout) -> std::optional<Callback>
{
    auto server   = httplib::Server{};
    auto mutex    = std::mutex{};
    auto arrived  = std::condition_variable_any{};
    auto callback = std::optional<Callback>{};

    server.set_keep_alive_max_count(1);
    server.set_read_timeout(kCallbackReadTimeout);
    server.set_error_handler([](const httplib::Request&, httplib::Response& response)
                             {
                                 if (response.body.empty())
                                 {
                                     servePage(response, httplib::StatusCode::NotFound_404, "TETRA MASTER", "This sign-in link is no longer valid.");
                                 }
                             });
    // The path is base64url, so it holds nothing the route pattern treats specially.
    server.Get(path, [&](const httplib::Request& request, httplib::Response& response)
               {
                   auto seen = callbackFrom(request);
                   if (seen.kind == Callback::Kind::other)
                   {
                       response.status = httplib::StatusCode::NotFound_404;
                       return;
                   }

                   if (seen.kind == Callback::Kind::error)
                   {
                       servePage(response, httplib::StatusCode::BadRequest_400, "Sign-in canceled", "Return to Tetra Master to try again. You can close this tab.");
                   }
                   else
                   {
                       servePage(response, httplib::StatusCode::OK_200, "You're signed in", "Returning to Tetra Master. You can close this tab.");
                   }

                   {
                       const auto lock = std::scoped_lock(mutex);
                       if (!callback)
                       {
                           callback = std::move(seen);
                       }
                   }

                   arrived.notify_all();
               });

    const auto port = server.bind_to_any_port("127.0.0.1");
    if (port < 0)
    {
        throw std::runtime_error("Cannot listen for the sign-in callback");
    }

    // Stopping lets requests in flight finish, so the browser still gets its page.
    const auto serving    = std::jthread([&server]
                                         {
                                          server.listen_after_bind();
                                         });
    const auto stopServer = wil::scope_exit([&server]
                                            {
                                                server.stop();
                                            });
    listening(static_cast<unsigned short>(port));

    auto lock = std::unique_lock(mutex);
    if (arrived.wait_for(lock, stop, timeout, [&callback]
                         {
                             return callback.has_value();
                         }))
    {
        return callback;
    }

    if (stop.stop_requested())
    {
        return std::nullopt;
    }

    throw std::runtime_error("Sign-in timed out");
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

auto AuthClient::signIn(const OpenBrowser& openBrowser, const std::stop_token& stop) -> std::optional<Tokens>
{
    const auto& auth     = settings();
    const auto  verifier = makeVerifier();
    const auto  path     = "/callback/" + base64Url(randomBytes(16));
    const auto  callback = awaitCallback(
        path,
        [&](unsigned short port)
        {
            const auto redirect = std::format("http://127.0.0.1:{}{}", port, path);
            openBrowser(std::format("{}/auth/v1/authorize?provider=discord&redirect_to={}&code_challenge={}&code_challenge_method=s256",
                                    auth.supabaseUrl,
                                    percentEncode(redirect),
                                    percentEncode(challengeFor(verifier))));
        },
        stop,
        kSignInTimeout);
    if (!callback)
    {
        return std::nullopt;
    }

    if (callback->kind == Callback::Kind::error)
    {
        throw std::runtime_error("Sign-in was canceled: " + callback->value);
    }

    const auto body = nlohmann::json{ { "auth_code", callback->value }, { "code_verifier", verifier } }.dump();
    return parseTokens(http_.send("POST", auth.supabaseUrl + "/auth/v1/token?grant_type=pkce", { { "apikey", auth.publishableKey }, { "Content-Type", "application/json" } }, body));
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
    target_ = widen(std::format("tmloader:{}|{}", origin, supabaseUrl));
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
    // CREDENTIALW takes mutable pointers.
    auto target                   = target_;
    auto blob                     = refreshToken;
    auto user                     = std::wstring{ L"tmloader" };
    auto credential               = CREDENTIALW{};
    credential.Type               = CRED_TYPE_GENERIC;
    credential.TargetName         = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob     = reinterpret_cast<BYTE*>(blob.data());
    credential.Persist            = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName           = user.data();
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
