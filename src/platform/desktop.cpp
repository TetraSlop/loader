#include "app/runtime.h"
#include <cstring>
#include <stdexcept>

namespace tmloader
{
namespace
{

// These adapters replace only imports in the supported game DLL. Windows,
// Dear ImGui and the loader itself retain their normal APIs.
auto WINAPI desktopKeyboardHook(int kind, HOOKPROC callback, HINSTANCE instance, DWORD thread) -> HHOOK
{
    if (kind == WH_KEYBOARD_LL)
    {
        logf("Skipped legacy global keyboard hook (desktop shortcuts remain available)");
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    return SetWindowsHookExA(kind, callback, instance, thread);
}

auto WINAPI desktopCursorPosition(int, int) -> BOOL
{
    return TRUE;
}

auto WINAPI desktopCursorVisibility(BOOL show) -> int
{
    // Native code loops until ShowCursor crosses zero. Satisfy that contract
    // without changing Windows' thread-local visibility counter.
    return show ? 0 : -1;
}

auto WINAPI desktopAttachInput(DWORD, DWORD, BOOL) -> BOOL
{
    return TRUE;
}

auto WINAPI desktopForeground(HWND) -> BOOL
{
    return TRUE;
}

auto WINAPI desktopActiveWindow(HWND window) -> HWND
{
    return window;
}

auto WINAPI desktopParameters(UINT action, UINT value, PVOID data, UINT flags) -> BOOL
{
    switch (action)
    {
        case SPI_SETANIMATION:
        case SPI_SETFOREGROUNDLOCKTIMEOUT:
        case SPI_SETSCREENSAVEACTIVE:
            return TRUE;
        case SPI_ICONHORIZONTALSPACING:
            if (!data)
                return TRUE; // A null output pointer selects the setter.
            break;
    }
    return SystemParametersInfoA(action, value, data, flags);
}

} // namespace

void skipLegacyCursorSync(HMODULE module)
{
    // TM +185753..+185830 copies its fixed 640x480 software cursor back to
    // the Windows pointer every frame, even while another window has focus.
    // Our input bridge already maps the pointer to the scaled game content.
    // Skip the whole reconciliation block, including its stale bounds clamp.
    auto* const base = reinterpret_cast<BYTE*>(module);
    BYTE* const site = base + 0x185753;
    DWORD       operand;
    std::memcpy(&operand, site + 1, sizeof(operand));
    if (site[0] != 0xa1 || operand != reinterpret_cast<DWORD>(base + 0x2ff248))
        throw std::runtime_error("Unsupported live cursor synchronization layout");
    DWORD protection;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protection))
        throw std::runtime_error("Cannot disable legacy cursor synchronization");
    const BYTE jump[] = { 0xe9, 0xd8, 0, 0, 0 }; // Relative branch to +185830.
    std::memcpy(site, jump, sizeof(jump));
    DWORD ignored;
    VirtualProtect(site, 5, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
}

void installDesktopHooks(HMODULE module)
{
    skipLegacyCursorSync(module);
    setSlot(module, 0x2041e4, reinterpret_cast<void*>(desktopKeyboardHook));
    setSlot(module, 0x2041e8, reinterpret_cast<void*>(desktopParameters));
    setSlot(module, 0x2041f0, reinterpret_cast<void*>(desktopCursorPosition));
    setSlot(module, 0x204240, reinterpret_cast<void*>(desktopAttachInput));
    setSlot(module, 0x204244, reinterpret_cast<void*>(desktopForeground));
    setSlot(module, 0x204248, reinterpret_cast<void*>(desktopCursorVisibility));
    setSlot(module, 0x204258, reinterpret_cast<void*>(desktopActiveWindow));
    logf("Desktop input policy installed: no global key interception, cursor warping or focus reclaim");
}

} // namespace tmloader
