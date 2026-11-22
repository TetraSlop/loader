#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tmloader
{

// The client always dials Game IRC on this port; the loader redirects it.
constexpr unsigned short kClientGamePort = 51241;

struct Config
{
    std::filesystem::path runtimeDir;
    std::filesystem::path game;
    std::filesystem::path viewer;
    std::filesystem::path log;
    // The --config file, or empty when settings came from the command line only.
    std::filesystem::path configFile;
    std::string           server = "127.0.0.1";
    // Empty until the player enters an account; the loader then asks for one.
    std::string username;
    std::string password;
    // The POL ID the game runs as: assigned by the server at sign-in, or a
    // built-in test account's with localAccount. Never read from or saved to
    // the config file otherwise.
    std::string    polId;
    unsigned short gamePort    = kClientGamePort;
    unsigned short profilePort = 51220;
    // A built-in test account on a loopback server: no LOGIN step, and the
    // account defaults to POL ID EEEEEEEF with password tm-local-test.
    bool localAccount = false;
    bool diagnostics  = false;
    bool checkOnly    = false;
    bool help         = false;
    bool version      = false;
};

auto parseConfig(const std::vector<std::string>& args) -> Config;
auto usage() -> std::string;
// Account and server rules shared by the config parser and the account prompt.
auto isValidPolId(const std::string& polId) -> bool;
// 1 to 15 of A-Z, a-z, 0-9, '_', '.' and '-'.
auto isValidUsername(const std::string& username) -> bool;
// 1 to 15 printable ASCII characters; polcore fails on longer passwords.
auto isValidPassword(const std::string& password) -> bool;
// A numeric IPv4 address or a DNS host name.
auto isValidServer(const std::string& server) -> bool;
auto isLoopbackServer(const std::string& server) -> bool;
// Sets each key in a key=value config file, keeping its other lines; creates the file if needed.
void saveConfigValues(const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& values);
// Reads only; never loads a DLL, creates a window or changes the token.
void validateRuntime(const Config& config);
auto sha256(const void* data, size_t bytes) -> std::array<std::uint8_t, 32>;
auto sha256Hex(const void* data, size_t bytes) -> std::string;
auto sha256File(const std::filesystem::path& path) -> std::string;

// What checkRetailFiles found, by runtime-relative path.
struct RetailFileReport
{
    size_t                   checked = 0;
    std::vector<std::string> missing;
    std::vector<std::string> mismatched;
    // Differs from the list but may change in normal use: the Viewer rewrites its settings file.
    std::vector<std::string> changed;
};

// Hashes every file a sha256sum-format list (retail-files.txt) names under runtimeDir.
// Throws on a malformed line or a path that leaves the runtime folder.
auto checkRetailFiles(const std::filesystem::path& list, const std::filesystem::path& runtimeDir) -> RetailFileReport;
// One line per missing or mismatched file, at most maxLines then a count of the rest; empty when none.
auto describeRetailFailures(const RetailFileReport& report, size_t maxLines = 20) -> std::string;
auto isProfileAlias(const char* host) -> bool;
auto isIpv4Address(const std::string& host) -> bool;
auto pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) -> bool;
auto outputPathAllowed(const Config& config, const std::filesystem::path& path) -> bool;
auto settingsPathAllowed(const Config& config, const std::filesystem::path& path) -> bool;

} // namespace tmloader
