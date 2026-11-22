#pragma once
#include "hosted/http.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

// Supabase PKCE sign-in and play-session API, per tmserver's LOADER_AUTH_SPEC.
namespace tmloader::hosted
{

// An HTTP error from the server or Supabase, carrying its {"error": code} when present.
class ApiError : public std::runtime_error
{
public:
    ApiError(DWORD status, std::string code);

    auto status() const noexcept -> DWORD;
    auto code() const noexcept -> const std::string&;

private:
    DWORD       status_;
    std::string code_;
};

struct AuthSettings
{
    std::string supabaseUrl;
    std::string publishableKey;
    std::string subprotocol;
};

struct Tokens
{
    std::string                           access;
    std::string                           refresh;
    std::chrono::system_clock::time_point expiresAt;
    // Supabase user UUID; the server operator enrolls this, never a display name.
    std::string userId;
    // Discord profile for display only.
    std::string displayName;
    std::string avatarUrl;
};

struct PlaySession
{
    std::string          token;
    std::chrono::seconds expiresIn{};
    std::string          polId;
    std::string          password;
    std::string          gameStream;
    std::string          profileStream;
};

// What the loopback listener saw for the pending sign-in.
struct Callback
{
    enum class Kind
    {
        other,
        code,
        error,
    };

    Kind        kind = Kind::other;
    std::string value;
};

auto base64Url(std::span<const std::uint8_t> bytes) -> std::string;
auto makeVerifier() -> std::string;
auto challengeFor(std::string_view verifier) -> std::string;
// Serves the callback on 127.0.0.1 and reports the chosen port through `listening`. Returns the
// code or error the browser brought to `path`, or nothing once `stop` is requested; throws on timeout.
auto awaitCallback(const std::string& path, const std::function<void(unsigned short port)>& listening, const std::stop_token& stop, std::chrono::steady_clock::duration timeout) -> std::optional<Callback>;
auto parseAuthSettings(const Response& response) -> AuthSettings;
auto parseTokens(const Response& response) -> Tokens;
auto parsePlaySession(const Response& response) -> PlaySession;

using OpenBrowser = std::function<void(const std::string& url)>;

class AuthClient
{
public:
    AuthClient(HttpClient& http, std::string origin);

    auto settings() -> const AuthSettings&;
    // Runs the browser PKCE flow; returns nothing when canceled.
    auto signIn(const OpenBrowser& openBrowser, const std::stop_token& stop) -> std::optional<Tokens>;
    auto refresh(const std::string& refreshToken) -> Tokens;

    auto issueTicket(const std::string& access) -> std::string;
    auto redeem(const std::string& ticket) -> PlaySession;
    void renew(const std::string& access, const std::string& sessionToken);
    void revokeSession(const std::string& sessionToken);
    void revokeAccountSession(const std::string& access);

private:
    auto call(const char* method, const std::string& path, const std::string& bearer, const std::string& body) -> Response;

    HttpClient&                 http_;
    std::string                 origin_;
    std::optional<AuthSettings> settings_;
};

// A refresh token in Windows Credential Manager, scoped to server origin and Supabase project.
class CredentialStore
{
public:
    CredentialStore(std::string_view origin, std::string_view supabaseUrl);

    auto load() const -> std::optional<std::string>;
    void save(const std::string& refreshToken) const;
    void erase() const noexcept;

private:
    std::wstring target_;
};

} // namespace tmloader::hosted
