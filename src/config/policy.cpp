#include <winsock2.h>

#include "config/config.h"
#include <algorithm>
#include <regex>
#include <wil/resource.h>
#include <ws2tcpip.h>

namespace tmloader
{

// The client resolves its profile servers as pp000.pol.com through pp999.pol.com.
auto isProfileAlias(const char* host) -> bool
{
    static const auto alias = std::regex(R"(pp\d{3}\.pol\.com)", std::regex::icase);
    return host && std::regex_match(host, alias);
}

auto isIpv4Address(const std::string& host) -> bool
{
    auto address = IN_ADDR{};
    return InetPtonA(AF_INET, host.c_str(), &address) == 1 && address.s_addr != INADDR_NONE && address.s_addr != INADDR_ANY;
}

auto isAllowedOrigin(const std::string& origin, bool insecureLoopback) -> bool
{
    static const auto secure   = std::regex(R"(https://[A-Za-z0-9.-]+(:\d{1,5})?)");
    static const auto loopback = std::regex(R"(http://127\.0\.0\.1:\d{1,5})");
    return std::regex_match(origin, secure) || (insecureLoopback && std::regex_match(origin, loopback));
}

auto pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) -> bool
{
    auto       error     = std::error_code{};
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error)
    {
        return false;
    }

    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error)
    {
        return false;
    }

    const auto sameName = [](const std::filesystem::path& a, const std::filesystem::path& b)
    {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    };
    const auto [rootEnd, rest] = std::mismatch(canonicalRoot.begin(), canonicalRoot.end(), canonical.begin(), canonical.end(), sameName);
    return rootEnd == canonicalRoot.end() && rest != canonical.end();
}

namespace
{

auto localOutputAllowed(const Config& config, const std::filesystem::path& path, const char* directory) -> bool
{
    // Boundaries are checked on resolved paths, so junctions and symlinks cannot escape.
    if (!pathIsWithin(path, config.runtimeDir / directory) || !pathIsWithin(path, config.runtimeDir) ||
        pathIsWithin(path, config.runtimeDir / "data") || pathIsWithin(path, config.runtimeDir / "pub"))
    {
        return false;
    }

    std::error_code ec;
    const auto      resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        return false;
    }

    for (const auto& input : { config.game, config.core })
    {
        const auto canonicalInput = std::filesystem::weakly_canonical(input, ec);
        if (ec || !_wcsicmp(resolved.c_str(), canonicalInput.c_str()))
        {
            return false;
        }
    }

    // A hard link inside logs could alias a DLL or asset elsewhere.
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        return false;
    }

    const wil::unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file)
    {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    const auto                 ok = GetFileInformationByHandle(file.get(), &info);
    return ok && info.nNumberOfLinks == 1;
}

} // namespace

auto outputPathAllowed(const Config& config, const std::filesystem::path& path) -> bool
{
    return localOutputAllowed(config, path, "logs");
}

auto settingsPathAllowed(const Config& config, const std::filesystem::path& path) -> bool
{
    return localOutputAllowed(config, path, "settings");
}

} // namespace tmloader
