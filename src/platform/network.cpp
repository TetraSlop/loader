#include "app/abi.h"
#include "app/hooks.h"
#include "app/log.h"
#include "platform/patch.h"
#include <cstring>

namespace tmloader
{
namespace
{

// Viewer internals. Unnamed functions are only known by call order.
constexpr DWORD kGameIrcSlots        = 0x3e58a0;
constexpr DWORD kGameIrcSlotSize     = 0x3a00;
constexpr DWORD kGameIrcCipherKeys   = 0x2b8;
constexpr DWORD kGameIrcCipher       = 0x2c0;
constexpr DWORD kCipherMatrixPointer = 0x48;
constexpr DWORD kProfileInitialized  = 0xafbd8;
constexpr DWORD kProfileEndpoint     = 0x404ab8;
constexpr DWORD kInitializeInet      = 0xfe20;
constexpr DWORD kResolveAddress      = 0x127a0;
constexpr DWORD kSeedProfileCipher   = 0x1a170;
constexpr DWORD kInitializeProfile   = 0x1eb00;
constexpr DWORD kSetProfileLogin     = 0x1ea60;
constexpr DWORD kSetProfileEndpoint  = 0x1e930;
constexpr DWORD kSub1E980            = 0x1e980;
constexpr DWORD kSub19BA0            = 0x19ba0;

constexpr DWORD kProfileRequestTimeoutMs = 5000;

Command original[abi::viewer::kTableSize]{};
BYTE*   viewerBase    = nullptr;
bool    profileLoaded = false;
// Game IRC slots are reused after a match; the profile cipher and its matrix must outlive them.
BYTE profileCipher[0x68]{};
BYTE profileMatrix[0x1000]{};

auto viewerFunction(DWORD rva) -> Command
{
    return reinterpret_cast<Command>(viewerBase + rva);
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
    if (port == kClientGamePort)
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
    return original[abi::viewer::kGameIrcOpen](options.server.c_str(), options.polId.c_str(), options.password.c_str(), version, mode, callbacks, cipher, queue, count);
}

void seedProfile(int slot)
{
    BYTE* const irc = viewerBase + kGameIrcSlots + slot * kGameIrcSlotSize;
    std::memcpy(profileCipher, irc + kGameIrcCipher, sizeof(profileCipher));
    auto*& matrix = *reinterpret_cast<void**>(profileCipher + kCipherMatrixPointer);
    std::memcpy(profileMatrix, matrix, sizeof(profileMatrix));
    matrix = profileMatrix;

    const auto* const keys = reinterpret_cast<const DWORD*>(irc + kGameIrcCipherKeys);
    viewerFunction(kSeedProfileCipher)(profileCipher, keys[0], keys[1]);
    viewerFunction(kSub1E980)();
}

void loadProfile()
{
    // Each request returns a ticket polled by the next table entry.
    for (const auto request : { abi::viewer::kLoadHandle, abi::viewer::kLoadCharacter, abi::viewer::kLoadStatus })
    {
        const int  ticket  = original[request]();
        int        status  = 0;
        const auto started = GetTickCount();
        while (ticket >= 0 && !status && GetTickCount() - started < kProfileRequestTimeoutMs)
        {
            status = original[request + 1](ticket);
            Sleep(10);
        }

        logLine("Profile bootstrap Common{} ticket={} result={}", request, ticket, status);
    }
}

auto __cdecl hookedGameIrcOpenCheck(int slot) -> int
{
    const int result = original[abi::viewer::kGameIrcOpenCheck](slot);
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
    const int result = original[abi::viewer::kProfileReadCheck](ticket);
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

void initializeViewerSession(HMODULE viewer, Command* table)
{
    std::memcpy(original, table, sizeof(original));
    viewerBase = reinterpret_cast<BYTE*>(viewer);

    // The game caches the account identity, so the Viewer must know it first.
    BYTE encodedAccount[8]{};
    original[abi::viewer::kEncodeAccount](encodedAccount, options.polId.c_str());
    original[abi::viewer::kSetAccount](encodedAccount);
    logLine("Viewer account context initialized before game caches its identity");
    logLine("Native INET initialization={}", viewerFunction(kInitializeInet)());

    BYTE endpoint[20]{};
    viewerFunction(kResolveAddress)(options.server.c_str(), endpoint);
    *reinterpret_cast<WORD*>(endpoint + 2) = options.profilePort;
    if (!*reinterpret_cast<DWORD*>(viewerBase + kProfileInitialized))
    {
        logLine("Profile initialization={}", viewerFunction(kInitializeProfile)(endpoint, options.polId.c_str(), options.password.c_str()));
    }
    else
    {
        viewerFunction(kSetProfileLogin)(options.polId.c_str(), options.password.c_str());
    }

    viewerFunction(kSetProfileEndpoint)(endpoint);
    std::memcpy(viewerBase + kProfileEndpoint, endpoint, sizeof(endpoint));
    viewerFunction(kSub19BA0)();

    replacePointer(viewer, reinterpret_cast<void*>(table[abi::viewer::kGameIrcOpen]), reinterpret_cast<void*>(hookedGameIrcOpen));
    replacePointer(viewer, reinterpret_cast<void*>(table[abi::viewer::kGameIrcOpenCheck]), reinterpret_cast<void*>(hookedGameIrcOpenCheck));
    replacePointer(viewer, reinterpret_cast<void*>(table[abi::viewer::kProfileReadCheck]), reinterpret_cast<void*>(hookedProfileReadCheck));
}

} // namespace tmloader
