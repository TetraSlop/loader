#include "app/abi.h"
#include "app/hooks.h"
#include "app/log.h"
#include "platform/patch.h"
#include <cstring>

namespace tmloader
{
namespace
{

// The client hardcodes this Game IRC port; it is redirected to the configured one.
constexpr u_short legacyGamePort = 51241;

// polcore_native.dll internals. Unnamed functions are only known by call order.
constexpr DWORD gameIrcSlots        = 0x3e58a0;
constexpr DWORD gameIrcSlotSize     = 0x3a00;
constexpr DWORD gameIrcCipherKeys   = 0x2b8;
constexpr DWORD gameIrcCipher       = 0x2c0;
constexpr DWORD cipherMatrixPointer = 0x48;
constexpr DWORD profileInitialized  = 0xafbd8;
constexpr DWORD profileEndpoint     = 0x404ab8;
constexpr DWORD initializeInet      = 0xfe20;
constexpr DWORD resolveAddress      = 0x127a0;
constexpr DWORD seedProfileCipher   = 0x1a170;
constexpr DWORD initializeProfile   = 0x1eb00;
constexpr DWORD setProfileLogin     = 0x1ea60;
constexpr DWORD setProfileEndpoint  = 0x1e930;
constexpr DWORD sub1E980            = 0x1e980;
constexpr DWORD sub19BA0            = 0x19ba0;

constexpr DWORD profileRequestTimeoutMs = 5000;

Command original[abi::viewer::tableSize]{};
BYTE*   coreBase      = nullptr;
bool    profileLoaded = false;
// Game IRC slots are reused after a match; the profile cipher and its matrix must outlive them.
BYTE profileCipher[0x68]{};
BYTE profileMatrix[0x1000]{};

auto core(DWORD rva) -> Command
{
    return reinterpret_cast<Command>(coreBase + rva);
}

auto WSAAPI hookedConnect(SOCKET socket, const sockaddr* address, int length) -> int
{
    if (!address || length < static_cast<int>(sizeof(sockaddr_in)) || address->sa_family != AF_INET)
    {
        WSASetLastError(WSAENETDOWN);
        return SOCKET_ERROR;
    }

    auto target = *reinterpret_cast<const sockaddr_in*>(address);
    auto port   = ntohs(target.sin_port);
    if (port == legacyGamePort)
    {
        port = options.gamePort;
    }

    if (target.sin_addr.s_addr != inet_addr(options.server.c_str()) || (port != options.gamePort && port != options.profilePort))
    {
        logLine("Blocked connection outside configured service endpoints");
        WSASetLastError(WSAENETDOWN);
        return SOCKET_ERROR;
    }

    target.sin_port = htons(port);
    return connect(socket, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
}

auto WSAAPI hookedGetHostByName(const char* host) -> hostent*
{
    if (isProfileAlias(host) || (host && options.server == host))
    {
        logLine("Profile/service hostname mapped: {}", host);
        return gethostbyname(options.server.c_str());
    }

    logLine("Blocked unconfigured hostname: {}", printable(host));
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}

auto __cdecl hookedGameIrcOpen(const char*, const char*, const char*, const char* version, int mode, void* callbacks, void* cipher, void* queue, int count) -> int
{
    logLine("GameIRC open to configured server; version={} mode={}", version, mode);
    return original[abi::viewer::gameIrcOpen](options.server.c_str(), options.polId.c_str(), options.password.c_str(), version, mode, callbacks, cipher, queue, count);
}

void seedProfile(int slot)
{
    BYTE* const irc = coreBase + gameIrcSlots + slot * gameIrcSlotSize;
    std::memcpy(profileCipher, irc + gameIrcCipher, sizeof(profileCipher));
    auto*& matrix = *reinterpret_cast<void**>(profileCipher + cipherMatrixPointer);
    std::memcpy(profileMatrix, matrix, sizeof(profileMatrix));
    matrix = profileMatrix;

    const auto* const keys = reinterpret_cast<const DWORD*>(irc + gameIrcCipherKeys);
    core(seedProfileCipher)(profileCipher, keys[0], keys[1]);
    core(sub1E980)();
}

void loadProfile()
{
    // Each request returns a ticket polled by the next table entry.
    for (const auto request : { abi::viewer::loadHandle, abi::viewer::loadCharacter, abi::viewer::loadStatus })
    {
        const int  ticket  = original[request]();
        int        status  = 0;
        const auto started = GetTickCount();
        while (ticket >= 0 && !status && GetTickCount() - started < profileRequestTimeoutMs)
        {
            status = original[request + 1](ticket);
            Sleep(10);
        }

        logLine("Profile bootstrap Common{} ticket={} result={}", request, ticket, status);
    }
}

auto __cdecl hookedGameIrcOpenCheck(int slot) -> int
{
    const int result = original[abi::viewer::gameIrcOpenCheck](slot);
    if (result == 1 && !profileLoaded)
    {
        profileLoaded = true;
        seedProfile(slot);
        loadProfile();
    }

    return result;
}

auto __cdecl hookedProfileReadCheck(int ticket) -> int
{
    const int result = original[abi::viewer::profileReadCheck](ticket);
    if (options.diagnostics)
    {
        logLine("Profile read ticket={} result={}", ticket, result);
    }

    return result;
}

} // namespace

void installNetworkHooks(HMODULE module)
{
    replacePointer(module, reinterpret_cast<void*>(connect), reinterpret_cast<void*>(hookedConnect));
    replacePointer(module, reinterpret_cast<void*>(gethostbyname), reinterpret_cast<void*>(hookedGetHostByName));
}

void initializeViewerSession(HMODULE module, Command* table)
{
    std::memcpy(original, table, sizeof(original));
    coreBase = reinterpret_cast<BYTE*>(module);

    // The game caches the account identity, so the Viewer must know it first.
    BYTE encodedAccount[8]{};
    original[abi::viewer::encodeAccount](encodedAccount, options.polId.c_str());
    original[abi::viewer::setAccount](encodedAccount);
    logLine("Viewer account context initialized before game caches its identity");
    logLine("Native INET initialization={}", core(initializeInet)());

    BYTE endpoint[20]{};
    core(resolveAddress)(options.server.c_str(), endpoint);
    *reinterpret_cast<WORD*>(endpoint + 2) = options.profilePort;
    if (!*reinterpret_cast<DWORD*>(coreBase + profileInitialized))
    {
        logLine("Profile initialization={}", core(initializeProfile)(endpoint, options.polId.c_str(), options.password.c_str()));
    }
    else
    {
        core(setProfileLogin)(options.polId.c_str(), options.password.c_str());
    }

    core(setProfileEndpoint)(endpoint);
    std::memcpy(coreBase + profileEndpoint, endpoint, sizeof(endpoint));
    core(sub19BA0)();

    replacePointer(module, reinterpret_cast<void*>(table[abi::viewer::gameIrcOpen]), reinterpret_cast<void*>(hookedGameIrcOpen));
    replacePointer(module, reinterpret_cast<void*>(table[abi::viewer::gameIrcOpenCheck]), reinterpret_cast<void*>(hookedGameIrcOpenCheck));
    replacePointer(module, reinterpret_cast<void*>(table[abi::viewer::profileReadCheck]), reinterpret_cast<void*>(hookedProfileReadCheck));
}

} // namespace tmloader
