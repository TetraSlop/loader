#include "app/hooks.h"
#include "app/log.h"
#include "platform/patch.h"
#include <cstring>
#include <stdexcept>

// Makes the fullscreen-era game behave as a desktop window by replacing its imports.
namespace tmloader
{
namespace
{

constexpr DWORD setWindowsHookImport      = 0x2041e4;
constexpr DWORD systemParametersImport    = 0x2041e8;
constexpr DWORD setCursorPosImport        = 0x2041f0;
constexpr DWORD attachThreadInputImport   = 0x204240;
constexpr DWORD setForegroundWindowImport = 0x204244;
constexpr DWORD showCursorImport          = 0x204248;
constexpr DWORD setActiveWindowImport     = 0x204258;
constexpr DWORD cursorSyncBegin           = 0x185753;
constexpr DWORD cursorSyncEnd             = 0x185830;
constexpr DWORD windowedFlag              = 0x2ff248;

auto WINAPI hookedSetWindowsHookExA(int kind, HOOKPROC callback, HINSTANCE instance, DWORD thread) -> HHOOK
{
    if (kind == WH_KEYBOARD_LL)
    {
        logLine("Skipped legacy global keyboard hook (desktop shortcuts remain available)");
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    return SetWindowsHookExA(kind, callback, instance, thread);
}

auto WINAPI hookedSetCursorPos(int, int) -> BOOL
{
    return TRUE;
}

auto WINAPI hookedShowCursor(BOOL show) -> int
{
    // The game loops until the counter crosses zero; fake it without touching the real one.
    if (show)
    {
        return 0;
    }

    return -1;
}

auto WINAPI hookedAttachThreadInput(DWORD, DWORD, BOOL) -> BOOL
{
    return TRUE;
}

auto WINAPI hookedSetForegroundWindow(HWND) -> BOOL
{
    return TRUE;
}

auto WINAPI hookedSetActiveWindow(HWND window) -> HWND
{
    return window;
}

auto WINAPI hookedSystemParametersInfoA(UINT action, UINT value, PVOID data, UINT flags) -> BOOL
{
    switch (action)
    {
        case SPI_SETANIMATION:
        case SPI_SETFOREGROUNDLOCKTIMEOUT:
        case SPI_SETSCREENSAVEACTIVE:
            return TRUE;
        case SPI_ICONHORIZONTALSPACING:
            // A null output pointer selects the setter form.
            if (!data)
            {
                return TRUE;
            }

            break;
    }

    return SystemParametersInfoA(action, value, data, flags);
}

void skipLegacyCursorSync(HMODULE module)
{
    // Each frame the game warps the OS pointer to its 640x480 cursor; jump over that block.
    auto* const base    = reinterpret_cast<BYTE*>(module);
    BYTE* const site    = base + cursorSyncBegin;
    auto        operand = DWORD{};
    std::memcpy(&operand, site + 1, sizeof(operand));
    if (site[0] != 0xa1 || operand != reinterpret_cast<DWORD>(base + windowedFlag))
    {
        throw std::runtime_error("Unsupported live cursor synchronization layout");
    }

    auto protection = DWORD{};
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protection))
    {
        throw std::runtime_error("Cannot disable legacy cursor synchronization");
    }

    constexpr BYTE jump[] = { 0xe9, cursorSyncEnd - cursorSyncBegin - 5, 0, 0, 0 };
    std::memcpy(site, jump, sizeof(jump));
    VirtualProtect(site, 5, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
}

} // namespace

void installDesktopHooks(HMODULE game)
{
    skipLegacyCursorSync(game);
    setImport(game, setWindowsHookImport, reinterpret_cast<void*>(hookedSetWindowsHookExA));
    setImport(game, systemParametersImport, reinterpret_cast<void*>(hookedSystemParametersInfoA));
    setImport(game, setCursorPosImport, reinterpret_cast<void*>(hookedSetCursorPos));
    setImport(game, attachThreadInputImport, reinterpret_cast<void*>(hookedAttachThreadInput));
    setImport(game, setForegroundWindowImport, reinterpret_cast<void*>(hookedSetForegroundWindow));
    setImport(game, showCursorImport, reinterpret_cast<void*>(hookedShowCursor));
    setImport(game, setActiveWindowImport, reinterpret_cast<void*>(hookedSetActiveWindow));
    logLine("Desktop input policy installed: no global key interception, cursor warping or focus reclaim");
}

} // namespace tmloader
