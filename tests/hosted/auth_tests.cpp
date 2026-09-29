#include "config/config.h"
#include "hosted/auth.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Percent encoding round-trips callback URLs", "[hosted]")
{
    const auto url = std::string("http://127.0.0.1:5123/callback/a b~");
    REQUIRE(percentEncode(url) == "http%3A%2F%2F127.0.0.1%3A5123%2Fcallback%2Fa%20b~");
    REQUIRE(percentDecode(percentEncode(url)) == url);
    REQUIRE(percentDecode("a+b%2") == "a b%2");
}

TEST_CASE("Only the pending callback path yields a code", "[hosted]")
{
    const auto path = std::string_view{ "/callback/nonce" };
    const auto code = parseCallback("GET /callback/nonce?code=abc%2Fdef&state=x HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
    REQUIRE(code.kind == Callback::Kind::code);
    REQUIRE(code.value == "abc/def");

    const auto denied = parseCallback("GET /callback/nonce?error=access_denied&error_description=User+cancelled HTTP/1.1\r\n\r\n", path);
    REQUIRE(denied.kind == Callback::Kind::error);
    REQUIRE(denied.value == "User cancelled");

    for (const auto* const request : { "GET /callback/other?code=abc HTTP/1.1\r\n\r\n", "GET /callback/nonce HTTP/1.1\r\n\r\n", "POST /callback/nonce?code=abc HTTP/1.1\r\n\r\n", "GET /callback/nonce?code=abc", "GET /favicon.ico HTTP/1.1\r\n\r\n" })
    {
        CAPTURE(request);
        REQUIRE(parseCallback(request, path).kind == Callback::Kind::other);
    }
}

TEST_CASE("Server configuration accepts only the Discord protocol", "[hosted]")
{
    const auto valid    = jsonResponse(200, R"({"protocol":1,"provider":"discord","supabase_url":"https://project.supabase.co","publishable_key":"sb_publishable_x","ticket_ttl_seconds":60,"session_ttl_seconds":3600,"stream_subprotocol":"tm-stream-v1","max_chunk_bytes":65536})");
    const auto settings = parseAuthSettings(valid);
    REQUIRE(settings.supabaseUrl == "https://project.supabase.co");
    REQUIRE(settings.subprotocol == "tm-stream-v1");

    for (const auto* const body : { R"({"protocol":2,"provider":"discord","supabase_url":"https://p.supabase.co","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})",
                                    R"({"protocol":1,"provider":"email","supabase_url":"https://p.supabase.co","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})",
                                    R"({"protocol":1,"provider":"discord","supabase_url":"http://p.supabase.co","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})",
                                    R"({"protocol":1,"provider":"discord","supabase_url":"https://p.supabase.co/path","publishable_key":"k","stream_subprotocol":"tm-stream-v1"})" })
    {
        CAPTURE(body);
        REQUIRE_THROWS(parseAuthSettings(jsonResponse(200, body)));
    }

    // Proxy failures may not be JSON; they surface as the status alone.
    const auto proxy = apiError([]
                                {
                                    parseAuthSettings({ 502, "text/html", "<html>Bad gateway</html>" });
                                });
    REQUIRE(proxy.status() == 502);
    REQUIRE(proxy.code().empty());
}

TEST_CASE("Play-session responses and errors are parsed", "[hosted]")
{
    const auto session = parsePlaySession(jsonResponse(201, R"({"protocol":1,"session_token":"token","expires_in":3600,"account_key":"UGITW5T4U","legacy":{"pol_id":"EEEEEEEF","password":"tm-local-test"},"streams":{"game":"wss://tm.example.com/v1/streams/game","profile":"wss://tm.example.com/v1/streams/profile"}})"));
    REQUIRE(session.token == "token");
    REQUIRE(session.expiresIn == std::chrono::seconds(3600));
    REQUIRE(session.polId == "EEEEEEEF");
    REQUIRE(session.password == "tm-local-test");
    REQUIRE(session.gameStream == "wss://tm.example.com/v1/streams/game");

    const auto conflict = apiError([]
                                   {
                                       parsePlaySession(jsonResponse(409, R"({"error":"session_conflict"})"));
                                   });
    REQUIRE(conflict.status() == 409);
    REQUIRE(conflict.code() == "session_conflict");

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
    REQUIRE_THROWS(parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"","expires_in":3600})")));
    REQUIRE(tokens.userId.empty());
    const auto withUser = parseTokens(jsonResponse(200, R"({"access_token":"a","refresh_token":"r","expires_in":3600,"user":{"id":"11111111-1111-4111-8111-111111111111","email":"x@example.com"}})"));
    REQUIRE(withUser.userId == "11111111-1111-4111-8111-111111111111");
}

TEST_CASE("Hosted origins require HTTPS except explicit loopback development", "[hosted]")
{
    REQUIRE(isAllowedOrigin("https://tm.example.com", false));
    REQUIRE(isAllowedOrigin("https://tm.example.com:8443", false));
    for (const auto* const origin : { "https://tm.example.com/", "https://tm.example.com/path", "http://tm.example.com", "https://user@tm.example.com", "ftp://tm.example.com" })
    {
        CAPTURE(origin);
        REQUIRE_FALSE(isAllowedOrigin(origin, false));
    }

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
