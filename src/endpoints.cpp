#include "config.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wil/resource.h>
#include <cstring>

namespace tmloader {
bool isProfileAlias(const char* host) {
    if (!host || std::strlen(host) != 13 || _strnicmp(host, "pp", 2) || _stricmp(host + 5, ".pol.com")) return false;
    return host[2] >= '0' && host[2] <= '9' && host[3] >= '0' && host[3] <= '9' && host[4] >= '0' && host[4] <= '9';
}
bool isIpv4Address(const std::string& host) {
    IN_ADDR address{};
    return InetPtonA(AF_INET, host.c_str(), &address) == 1 && address.s_addr != INADDR_NONE && address.s_addr != INADDR_ANY;
}
bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    const auto p = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const auto r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    auto i = p.begin();
    for (auto j = r.begin(); j != r.end(); ++j, ++i)
        if (i == p.end() || _wcsicmp(i->c_str(), j->c_str())) return false;
    return i != p.end();
}

static bool localOutputAllowed(const Config& config, const std::filesystem::path& path, const char* directory) {
    // Resolve directory junctions/file symlinks before checking boundaries.
    // The dedicated output tree must itself remain within the runtime root.
    if (!pathIsWithin(path, config.runtimeDir / directory) || !pathIsWithin(path, config.runtimeDir) ||
        pathIsWithin(path, config.runtimeDir / "data") || pathIsWithin(path, config.runtimeDir / "pub")) return false;
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    for (const auto& input : {config.game, config.core}) {
        const auto canonicalInput = std::filesystem::weakly_canonical(input, ec);
        if (ec || !_wcsicmp(resolved.c_str(), canonicalInput.c_str())) return false;
    }
    // A hard link inside logs can still name a DLL or asset outside logs.
    // Refuse all multiply-linked existing output files, without writing them.
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    const wil::unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const auto ok = GetFileInformationByHandle(file.get(), &info);
    return ok && info.nNumberOfLinks == 1;
}
bool outputPathAllowed(const Config& config, const std::filesystem::path& path) {
    return localOutputAllowed(config, path, "logs");
}
bool settingsPathAllowed(const Config& config, const std::filesystem::path& path) {
    return localOutputAllowed(config, path, "settings");
}
}
