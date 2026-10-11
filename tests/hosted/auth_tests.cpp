#include <httplib.h>

#include "config/config.h"
#include "hosted/auth.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace
{

constexpr auto kCallbackPath    = "/callback/nonce";
constexpr auto kCallbackTimeout = std::chrono::seconds(30);

} // namespace

namespace tmloader
{

void writeLog(std::string_view)
{
}

} // namespace tmloader

using namespace tmloader;
using namespace tmloader::hosted;

namespace
{

auto jsonResponse(DWORD status, std::string body) -> Response
{
    return { status, "application/json", std::move(body) };
}

struct Reply
{
    int         status = 0;
    std::string body;
    std::string cacheControl;
};

struct Visit
{
    std::optional<Callback> callback;
    std::vector<Reply>      replies;
};

// Stands in for the browser: requests each target once the listener reports its port.
auto visit(const std::vector<std::string>& targets) -> Visit
{
    auto       result  = Visit{};
    auto       browser = std::jthread{};
    const auto stop    = std::stop_source{};
    result.callback    = awaitCallback(
        kCallbackPath,
        [&](unsigned short port)
        {
            browser = std::jthread([&result, &targets, port]
                                   {
                                       auto client = httplib::Client("127.0.0.1", port);
                                       for (const auto& target : targets)
                                       {
                                           if (const auto response = client.Get(target))
                                           {
                                               result.replies.push_back({ response->status, response->body, response->get_header_value("Cache-Control") });
                                           }
                                       }
                                   });
        },
        stop.get_token(),
        kCallbackTimeout);
    browser.join();
    return result;
}

template <typename Parse>
auto apiError(Parse parse) -> ApiError
{
    try
    {
        parse();
    }
    catch (const ApiError& error)
    {
        return error;
    }

    FAIL("expected an ApiError");
    return ApiError(0, "");
}

} // namespace

TEST_CASE("PKCE matches RFC 7636 and uses unreserved characters", "[hosted]")
{
    REQUIRE(challengeFor("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    REQUIRE(base64Url(std::array<std::uint8_t, 2>{ 0xfb, 0xff }) == "-_8");
    const auto verifier = makeVerifier();
    REQUIRE(verifier.size() == 43);
    REQUIRE(std::ranges::all_of(verifier, [](char c)
                                {
                                    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
                                }));
    REQUIRE(makeVerifier() != verifier);
}

TEST_CASE("Percent encoding keeps only unreserved characters", "[hosted]")
{
    const auto url = std::string("http://127.0.0.1:5123/callback/a b~");
    REQUIRE(percentEncode(url) == "http%3A%2F%2F127.0.0.1%3A5123%2Fcallback%2Fa%20b~");
}

TEST_CASE("The pending callback path yields its decoded code", "[hosted]")
{
    const auto seen = visit({ "/callback/nonce?code=abc%2Fdef+g&state=x" });
    REQUIRE(seen.callback);
    REQUIRE(seen.callback->kind == Callback::Kind::code);
    REQUIRE(seen.callback->value == "abc/def g");
    REQUIRE(seen.replies.size() == 1);
    REQUIRE(seen.replies[0].status == 200);
    REQUIRE(seen.replies[0].body.contains("You're signed in"));
    REQUIRE(seen.replies[0].cacheControl == "no-store");
}

TEST_CASE("A denied sign-in yields the provider's description", "[hosted]")
{
    const auto denied = visit({ "/callback/nonce?error=access_denied&error_description=User+cancelled" });
    REQUIRE(denied.callback);
    REQUIRE(denied.callback->kind == Callback::Kind::error);
    REQUIRE(denied.callback->value == "User cancelled");
    REQUIRE(denied.replies.size() == 1);
    REQUIRE(denied.replies[0].status == 400);
    REQUIRE(denied.replies[0].body.contains("Sign-in cancelled"));

    const auto bare = visit({ "/callback/nonce?error=access_denied" });
    REQUIRE(bare.callback);
    REQUIRE(bare.callback->kind == Callback::Kind::error);
    REQUIRE(bare.callback->value == "access_denied");

    const auto empty = visit({ "/callback/nonce?state=x" });
    REQUIRE(empty.callback);
    REQUIRE(empty.callback->kind == Callback::Kind::error);
    REQUIRE(empty.callback->value == "sign-in failed");
}

TEST_CASE("Requests that are not the pending callback do not end sign-in", "[hosted]")
{
    const auto seen = visit({ "/callback/other?code=wrong", "/callback/nonce", "/callback/nonce/more?code=wrong", "/favicon.ico", "/callback/nonce?code=right" });
    REQUIRE(seen.callback);
    REQUIRE(seen.callback->kind == Callback::Kind::code);
    REQUIRE(seen.callback->value == "right");
    REQUIRE(seen.replies.size() == 5);
    for (auto i = size_t{ 0 }; i < 4; ++i)
    {
        CAPTURE(i);
        REQUIRE(seen.replies[i].status == 404);
        REQUIRE(seen.replies[i].body.contains("This sign-in link is no longer valid."));
        REQUIRE(seen.replies[i].cacheControl == "no-store");
    }

    REQUIRE(seen.replies[4].status == 200);
}

TEST_CASE("Only a GET of the callback path is accepted", "[hosted]")
{
    auto       stop     = std::stop_source{};
    auto       statuses = std::vector<int>{};
    auto       browser  = std::jthread{};
    const auto callback = awaitCallback(
        kCallbackPath,
        [&](unsigned short port)
        {
            browser = std::jthread([&stop, &statuses, port]
                                   {
                                       auto client = httplib::Client("127.0.0.1", port);
                                       if (const auto response = client.Post("/callback/nonce?code=abc"))
                                       {
                                           statuses.push_back(response->status);
                                       }

                                       if (const auto response = client.Head("/callback/nonce?code=abc"))
                                       {
                                           statuses.push_back(response->status);
                                       }

                                       stop.request_stop();
                                   });
        },
        stop.get_token(),
        kCallbackTimeout);
    browser.join();
    REQUIRE(!callback);
    REQUIRE(statuses == std::vector{ 404, 404 });
}

TEST_CASE("Waiting for the callback ends on cancellation and on timeout", "[hosted]")
{
    auto stop = std::stop_source{};
    stop.request_stop();
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(!awaitCallback(kCallbackPath, [](unsigned short)
                           {
                           },
                           stop.get_token(),
                           kCallbackTimeout));
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));

    REQUIRE_THROWS_AS(awaitCallback(kCallbackPath, [](unsigned short)
                                    {
                                    },
                                    std::stop_token{},
                                    std::chrono::milliseconds(50)),
                      std::runtime_error);
}

TEST_CASE("Server configuration is read from the gateway", "[hosted]")
{
    const auto valid    = jsonResponse(200, R"({"protocol":1,"provider":"discord","supabase_url":"https://project.supabase.co","publishable_key":"sb_publishable_x","ticket_ttl_seconds":60,"session_ttl_seconds":3600,"stream_subprotocol":"tm-stream-v1","max_chunk_bytes":65536})");
    const auto settings = parseAuthSettings(valid);
    REQUIRE(settings.supabaseUrl == "https://project.supabase.co");
    REQUIRE(settings.subprotocol == "tm-stream-v1");
}

TEST_CASE("Server configuration accepts only the Discord protocol", "[hosted]")
{
    for (const auto* const body : { R"({"protocol":2,"provider":"discord","supabase_url":"https://p.supabase.co","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})",
                                    R"({"protocol":1,"provider":"email","supabase_url":"https://p.supabase.co","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})",
                                    R"({"protocol":1,"provider":"discord","supabase_url":"http://p.supabase.co","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})",
                                    R"({"protocol":1,"provider":"discord","supabase_url":"https://p.supabase.co/path","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})" })
    {
        CAPTURE(body);
        REQUIRE_THROWS(parseAuthSettings(jsonResponse(200, body)));
    }
}

TEST_CASE("A non-JSON gateway failure surfaces as its status alone", "[hosted]")
{
    const auto proxy = apiError([]
                                {
                                    parseAuthSettings({ 502, "text/html", "<html>Bad gateway</html>" });
                                });
    REQUIRE(proxy.status() == 502);
    REQUIRE(proxy.code().empty());
}

TEST_CASE("Play-session responses are parsed", "[hosted]")
{
    const auto session = parsePlaySession(jsonResponse(201, R"({"protocol":1,"session_token":"token","expires_in":3600,"account_key":"UGITW5T4U","legacy":{"pol_id":"EEEEEEEF","password":"tm-local-test"},"streams":{"game":"wss://tm.example.com/v1/streams/game","profile":"wss://tm.example.com/v1/streams/profile"}})"));
    REQUIRE(session.token == "token");
    REQUIRE(session.expiresIn == std::chrono::seconds(3600));
    REQUIRE(session.polId == "EEEEEEEF");
    REQUIRE(session.password == "tm-local-test");
    REQUIRE(session.gameStream == "wss://tm.example.com/v1/streams/game");
}

TEST_CASE("Gateway errors carry their status and code", "[hosted]")
{
    const auto conflict = apiError([]
                                   {
                                       parsePlaySession(jsonResponse(409, R"({"error":"session_conflict"})"));
                                   });
    REQUIRE(conflict.status() == 409);
    REQUIRE(conflict.code() == "session_conflict");
}

TEST_CASE("Supabase errors carry their error code", "[hosted]")
{
    const auto supabase = apiError([]
                                   {
                                       parseTokens(jsonResponse(400, R"({"code":400,"error_code":"refresh_token_not_found","msg":"Invalid Refresh Token"})"));
                                   });
    REQUIRE(supabase.code() == "refresh_token_not_found");
}

TEST_CASE("Token responses require both tokens", "[hosted]")
{
    const auto tokens = parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"r","expires_in":3600,"token_type":"bearer"})"));
    REQUIRE(tokens.access == "a");
    REQUIRE(tokens.refresh == "r");
    REQUIRE(tokens.expiresAt > std::chrono::system_clock::now());
    REQUIRE(tokens.userId.empty());
    REQUIRE_THROWS(parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"","expires_in":3600})")));
}

TEST_CASE("Token responses carry the user id when present", "[hosted]")
{
    const auto withUser = parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"r","expires_in":3600,"user":{"id":"11111111-1111-4111-8111-111111111111","email":"x@example.com"}})"));
    REQUIRE(withUser.userId == "11111111-1111-4111-8111-111111111111");
}

TEST_CASE("The Discord display name falls back to the account name", "[hosted]")
{
    const auto discord = parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"r","expires_in":3600,"user":{"id":"u","user_metadata":{"full_name":"account_name","avatar_url":"https://cdn.discordapp.com/avatars/1/a.png","custom_claims":{"global_name":"Display Name"}}}})"));
    REQUIRE(discord.displayName == "Display Name");
    REQUIRE(discord.avatarUrl == "https://cdn.discordapp.com/avatars/1/a.png");

    const auto fallback = parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"r","expires_in":3600,"user":{"id":"u","user_metadata":{"full_name":"account_name"}}})"));
    REQUIRE(fallback.displayName == "account_name");
    REQUIRE(fallback.avatarUrl.empty());
}

TEST_CASE("Hosted origins must be bare HTTPS origins", "[hosted]")
{
    REQUIRE(isAllowedOrigin("https://tm.example.com", false));
    REQUIRE(isAllowedOrigin("https://tm.example.com:8443", false));
    for (const auto* const origin : { "https://tm.example.com/", "https://tm.example.com/path", "http://tm.example.com", "https://user@tm.example.com", "ftp://tm.example.com" })
    {
        CAPTURE(origin);
        REQUIRE_FALSE(isAllowedOrigin(origin, false));
    }
}

TEST_CASE("Plain HTTP is allowed only for explicit loopback development", "[hosted]")
{
    REQUIRE_FALSE(isAllowedOrigin("http://127.0.0.1:8081", false));
    REQUIRE(isAllowedOrigin("http://127.0.0.1:8081", true));
    REQUIRE_FALSE(isAllowedOrigin("http://localhost:8081", true));
    REQUIRE_FALSE(isAllowedOrigin("http://10.0.0.1:8081", true));
}

TEST_CASE("Remembered credentials round-trip through Credential Manager", "[hosted]")
{
    const auto store = CredentialStore("https://tmloader-test.invalid", std::to_string(GetCurrentProcessId()));
    store.erase();
    REQUIRE_FALSE(store.load());
    store.save("refresh-token");
    REQUIRE(store.load() == "refresh-token");
    store.save("rotated-token");
    REQUIRE(store.load() == "rotated-token");
    store.erase();
    REQUIRE_FALSE(store.load());
}
