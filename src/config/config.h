#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace tmloader
{

struct Config
{
    std::filesystem::path runtimeDir;
    std::filesystem::path game;
    std::filesystem::path core;
    std::filesystem::path log;
    std::filesystem::path texturePack;
    std::string           server      = "127.0.0.1";
    std::string           polId       = "EEEEEEEF";
    std::string           password    = "tm-local-test";
    unsigned short        gamePort    = 51241;
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
auto sha256Bytes(const void* data, size_t bytes) -> std::string;
auto sha256File(const std::filesystem::path& path) -> std::string;
auto isProfileAlias(const char* host) -> bool;
auto isIpv4Address(const std::string& host) -> bool;
auto pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) -> bool;
auto outputPathAllowed(const Config& config, const std::filesystem::path& path) -> bool;
auto settingsPathAllowed(const Config& config, const std::filesystem::path& path) -> bool;

} // namespace tmloader
