#pragma once
#include <chrono>
#include <stdexcept>
#include <string>

namespace tmloader
{

// The account the server confirmed. gamePassword is what the client sends in
// place of the player's password; it stays in memory only.
struct LoginResult
{
    std::string polId;
    std::string gamePassword;
    bool        created = false;
};

// A refused LOGIN carries the server's 901 code; transport failures have none.
class LoginError : public std::runtime_error
{
public:
    LoginError(std::string code, const std::string& message);
    auto code() const -> const std::string&;

private:
    std::string code_;
};

// Sends the player's POL ID and password to the server's game port over its
// own key exchange, as tmserver's LOGIN verb expects. The exchange has no
// server authentication, so someone able to intercept the connection can read
// the password. Throws LoginError.
auto login(const std::string& server, unsigned short port, const std::string& polId, const std::string& password, std::chrono::milliseconds timeout = std::chrono::seconds(15)) -> LoginResult;

// What to tell the player about a failed LOGIN.
auto describeLoginError(const LoginError& error) -> std::string;

// The IPv4 address of a numeric address or host name; throws when it cannot be resolved.
auto resolveIpv4(const std::string& host) -> std::string;

} // namespace tmloader
