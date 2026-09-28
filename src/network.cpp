#include "runtime.h"
#include <cstring>

namespace tmloader
{
namespace
{

Command original[1200]{};
BYTE*   coreBase      = nullptr;
bool    profileLoaded = false;
BYTE    profileContext[0x68]{}, profileMatrix[0x1000]{};

auto WSAAPI serviceConnect(SOCKET socket, const sockaddr* address, int length) -> int
{
    if (!address || length < sizeof(sockaddr_in) || address->sa_family != AF_INET)
    {
        WSASetLastError(WSAENETDOWN);
        return SOCKET_ERROR;
    }
    sockaddr_in target = *reinterpret_cast<const sockaddr_in*>(address);
    auto        port   = ntohs(target.sin_port);
    // GameIRC uses the legacy fixed port; make the configured replacement
    // explicit. Profile setup below already supplies its configured port.
    if (port == 51241)
        port = options.gamePort;
    if (target.sin_addr.s_addr != inet_addr(options.server.c_str()) ||
        (port != options.gamePort && port != options.profilePort))
    {
        logf("Blocked connection outside configured service endpoints");
        WSASetLastError(WSAENETDOWN);
        return SOCKET_ERROR;
    }
    target.sin_port = htons(port);
    return connect(socket, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
}

auto WSAAPI serviceHost(const char* host) -> hostent*
{
    if (isProfileAlias(host) || (host && options.server == host))
    {
        logf("Profile/service hostname mapped: %s", host);
        return gethostbyname(options.server.c_str());
    }
    logf("Blocked unconfigured hostname: %s", host ? host : "(null)");
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}

auto __cdecl gameOpen(const char*, const char*, const char*, const char* version, int mode, void* callbacks, void* cipher, void* queue, int count) -> int
{
    logf("GameIRC open to configured server; version=%s mode=%d", version, mode);
    return original[132](options.server.c_str(), options.polId.c_str(), options.password.c_str(), version, mode, callbacks, cipher, queue, count);
}

auto __cdecl gameOpenCheck(int slot) -> int
{
    const int result = original[133](slot);
    if (result == 1 && !profileLoaded)
    {
        profileLoaded   = true;
        BYTE* const irc = coreBase + 0x3e58a0 + slot * 0x3a00;
        // The Viewer profile cipher must survive GameIRC slot reuse. Copy both
        // the 0x68-byte context AND the matrix referenced by context+0x48.
        std::memcpy(profileContext, irc + 0x2c0, sizeof(profileContext));
        std::memcpy(profileMatrix, *reinterpret_cast<void**>(profileContext + 0x48), sizeof(profileMatrix));
        *reinterpret_cast<void**>(profileContext + 0x48) = profileMatrix;
        reinterpret_cast<Command>(coreBase + 0x1a170)(profileContext,
                                                      *reinterpret_cast<DWORD*>(irc + 0x2b8),
                                                      *reinterpret_cast<DWORD*>(irc + 0x2bc));
        reinterpret_cast<Command>(coreBase + 0x1e980)();
        const unsigned requests[] = { 171, 175, 193 };
        for (const unsigned index : requests)
        {
            const int  ticket  = original[index]();
            int        status  = 0;
            const auto started = GetTickCount();
            while (ticket >= 0 && !status && GetTickCount() - started < 5000)
            {
                status = original[index + 1](ticket);
                Sleep(10);
            }
            logf("Profile bootstrap Common%u ticket=%d result=%d", index, ticket, status);
        }
    }
    return result;
}

auto __cdecl readCheck(int ticket) -> int
{
    const int result = original[349](ticket);
    if (options.diagnostics)
        logf("Profile read ticket=%d result=%d", ticket, result);
    return result;
}

} // namespace

void installNetworkHooks(HMODULE module)
{
    replacePointer(module, reinterpret_cast<void*>(connect), reinterpret_cast<void*>(serviceConnect));
    replacePointer(module, reinterpret_cast<void*>(gethostbyname), reinterpret_cast<void*>(serviceHost));
}

void initializeViewerSession(HMODULE module, Command* table)
{
    std::memcpy(original, table, sizeof(original));
    coreBase = reinterpret_cast<BYTE*>(module);
    BYTE encodedAccount[8]{};
    original[854](encodedAccount, options.polId.c_str());
    original[851](encodedAccount);
    logf("Viewer account context initialized before game caches its identity");
    logf("Native INET initialization=%d", reinterpret_cast<Command>(coreBase + 0xfe20)());

    BYTE address[20]{};
    reinterpret_cast<Command>(coreBase + 0x127a0)(options.server.c_str(), address);
    *reinterpret_cast<WORD*>(address + 2) = options.profilePort;
    if (!*reinterpret_cast<DWORD*>(coreBase + 0xafbd8))
        logf("Profile initialization=%d", reinterpret_cast<Command>(coreBase + 0x1eb00)(address, options.polId.c_str(), options.password.c_str()));
    else
        reinterpret_cast<Command>(coreBase + 0x1ea60)(options.polId.c_str(), options.password.c_str());
    reinterpret_cast<Command>(coreBase + 0x1e930)(address);
    std::memcpy(coreBase + 0x404ab8, address, sizeof(address));
    reinterpret_cast<Command>(coreBase + 0x19ba0)();
    replacePointer(module, reinterpret_cast<void*>(table[132]), reinterpret_cast<void*>(gameOpen));
    replacePointer(module, reinterpret_cast<void*>(table[133]), reinterpret_cast<void*>(gameOpenCheck));
    replacePointer(module, reinterpret_cast<void*>(table[349]), reinterpret_cast<void*>(readCheck));
}

} // namespace tmloader
