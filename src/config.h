#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace tmloader {
struct Config {
    std::filesystem::path runtimeDir, game, core, log;
    std::string server = "127.0.0.1";
    std::string polId = "EEEEEEEF";
    std::string password = "tm-local-test";
    unsigned short gamePort = 51241, profilePort = 51220;
    bool diagnostics = false;
    bool checkOnly = false, help = false, version = false;
};
Config parseConfig(const std::vector<std::string>& args);
const char* usage();
// Reads only; never loads a DLL, creates a window or changes the token.
void validateRuntime(const Config& config);
std::string sha256File(const std::filesystem::path& path);
bool isProfileAlias(const char* host);
bool isIpv4Address(const std::string& host);
bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root);
bool outputPathAllowed(const Config& config, const std::filesystem::path& path);
bool settingsPathAllowed(const Config& config, const std::filesystem::path& path);
}
