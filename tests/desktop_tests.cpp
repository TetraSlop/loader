#include "runtime.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <stdexcept>
static int  keyboardHooks, parameterCalls;
static UINT lastParameter;

static HHOOK fakeKeyboardHook(int, HOOKPROC, HINSTANCE, DWORD)
{
    ++keyboardHooks;
    return reinterpret_cast<HHOOK>(1);
}

static BOOL fakeParameters(UINT action, UINT, PVOID, UINT)
{
    ++parameterCalls;
    lastParameter = action;
    return TRUE;
}

#define SetWindowsHookExA     fakeKeyboardHook
#define SystemParametersInfoA fakeParameters
#include "../src/desktop.cpp"
#undef SetWindowsHookExA
#undef SystemParametersInfoA

namespace tmloader
{

static std::map<DWORD, void*> imports;

void logf(const char*, ...)
{
}

void setSlot(HMODULE, DWORD rva, void* function)
{
    imports[rva] = function;
}

} // namespace tmloader

TEST_CASE("Desktop hooks preserve user input and system settings", "[desktop]")
{
    using namespace tmloader;
    // In-memory fixture only: no game DLL, keyboard hook or desktop mutation.
    auto* base = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x310000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(base);
    auto module   = reinterpret_cast<HMODULE>(base);
    bool rejected = false;
    try
    {
        skipLegacyCursorSync(module);
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    REQUIRE((rejected && base[0x185753] == 0));
    BYTE* site    = base + 0x185753;
    site[-1]      = 0xcc;
    site[5]       = 0xcc;
    site[0]       = 0xa1;
    DWORD operand = reinterpret_cast<DWORD>(base + 0x2ff248);
    std::memcpy(site + 1, &operand, sizeof(operand));
    installDesktopHooks(module);
    LONG displacement;
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    REQUIRE((site[0] == 0xe9 && site + 5 + displacement == base + 0x185830));
    REQUIRE((site[-1] == 0xcc && site[5] == 0xcc && imports.size() == 7));

    auto keyHook = reinterpret_cast<decltype(&SetWindowsHookExA)>(imports.at(0x2041e4));
    REQUIRE((keyHook(WH_KEYBOARD_LL, nullptr, nullptr, 0) == nullptr && keyboardHooks == 0));
    REQUIRE((keyHook(WH_GETMESSAGE, nullptr, nullptr, 123) != nullptr && keyboardHooks == 1));
    auto parameters = reinterpret_cast<decltype(&SystemParametersInfoA)>(imports.at(0x2041e8));
    for (UINT action : { SPI_SETANIMATION, SPI_SETFOREGROUNDLOCKTIMEOUT, SPI_SETSCREENSAVEACTIVE, SPI_ICONHORIZONTALSPACING })
        REQUIRE(parameters(action, 0, nullptr, SPIF_SENDCHANGE));
    REQUIRE(parameterCalls == 0);
    DWORD output = 0;
    REQUIRE(parameters(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &output, 0));
    REQUIRE((parameterCalls == 1 && lastParameter == SPI_GETFOREGROUNDLOCKTIMEOUT));
    REQUIRE((parameters(SPI_ICONHORIZONTALSPACING, 0, &output, 0) && parameterCalls == 2));

    auto cursor = reinterpret_cast<decltype(&ShowCursor)>(imports.at(0x204248));
    for (int i = 0; i < 10; ++i)
        REQUIRE((cursor(FALSE) < 0 && cursor(TRUE) >= 0));
    auto warp = reinterpret_cast<decltype(&SetCursorPos)>(imports.at(0x2041f0));
    REQUIRE(warp(-9999, -9999));
    auto attach = reinterpret_cast<decltype(&AttachThreadInput)>(imports.at(0x204240));
    REQUIRE(attach(0, 0, TRUE));
    auto foreground = reinterpret_cast<decltype(&SetForegroundWindow)>(imports.at(0x204244));
    REQUIRE(foreground(nullptr));
    auto active = reinterpret_cast<decltype(&SetActiveWindow)>(imports.at(0x204258));
    HWND window = reinterpret_cast<HWND>(1);
    REQUIRE(active(window) == window);
    VirtualFree(base, 0, MEM_RELEASE);
}
