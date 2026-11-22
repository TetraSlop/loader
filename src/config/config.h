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
    std::string           server      = "127.0.0.1";
    std::string           polId       = "EEEEEEEF";
    std::string           password    = "tm-local-test";
    unsigned short        gamePort    = kClientGamePort;
    unsigned short        profilePort = 51220;
    bool                  diagnostics = false;
    bool                  checkOnly   = false;
    bool                  help        = false;
    bool                  version     = false;
};

auto parseConfig(const std::vector<std::string>& args) -> Config;
auto usage() -> std::string;
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
