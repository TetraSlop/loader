#include "config.h"
#include <winsock2.h>
#include <ws2tcpip.h>
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
    auto p = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    auto r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    auto i = p.begin();
    for (auto j = r.begin(); j != r.end(); ++j, ++i)
        if (i == p.end() || _wcsicmp(i->c_str(), j->c_str())) return false;
    return i != p.end();
}
}
