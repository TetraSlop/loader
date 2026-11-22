#include "app/runtime.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <stdexcept>

namespace
{

constexpr SIZE_T kImageSize       = 0x310000;
constexpr DWORD  kSyncSiteRva     = 0x185753;
constexpr DWORD  kSyncResumeRva   = 0x185830;
constexpr DWORD  kKeyboardHookRva = 0x2041e4;
constexpr DWORD  kParametersRva   = 0x2041e8;
constexpr DWORD  kCursorPosRva    = 0x2041f0;
constexpr DWORD  kAttachInputRva  = 0x204240;
constexpr DWORD  kForegroundRva   = 0x204244;
constexpr DWORD  kShowCursorRva   = 0x204248;
constexpr DWORD  kActiveWindowRva = 0x204258;
constexpr BYTE   kBreakpoint      = 0xcc;
constexpr BYTE   kLoadEaxAbsolute = 0xa1;
constexpr BYTE   kNearJump        = 0xe9;
constexpr int    kNearJumpLength  = 5;

int                    keyboardHooks;
int                    parameterCalls;
UINT                   lastParameter;
std::map<DWORD, void*> imports;

auto fakeKeyboardHook(int, HOOKPROC, HINSTANCE, DWORD) -> HHOOK
{
    ++keyboardHooks;
    return reinterpret_cast<HHOOK>(1);
}

auto fakeParameters(UINT action, UINT, PVOID, UINT) -> BOOL
{
    ++parameterCalls;
    lastParameter = action;
    return TRUE;
}

} // namespace

#define SetWindowsHookExA     fakeKeyboardHook
#define SystemParametersInfoA fakeParameters
#include "platform/desktop.cpp"
#undef SetWindowsHookExA
#undef SystemParametersInfoA

namespace tmloader
{

void writeLog(std::string_view)
{
}

void setImport(HMODULE, DWORD rva, void* function)
{
    imports[rva] = function;
}

void writeCode(void* site, std::span<const BYTE> bytes)
{
    std::memcpy(site, bytes.data(), bytes.size());
}

} // namespace tmloader

using namespace tmloader;

namespace
{

// In-memory stand-in for the game image: no game DLL, keyboard hook or desktop mutation.
auto allocateImage() -> BYTE*
{
    auto* const base = static_cast<BYTE*>(VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(base);
    return base;
}

// Writes the instruction the patch expects, `mov eax, [windowed flag]`, between two guard bytes.
void writeCursorSync(BYTE* base)
{
    BYTE* const site      = base + kSyncSiteRva;
    site[-1]              = kBreakpoint;
    site[kNearJumpLength] = kBreakpoint;
    site[0]               = kLoadEaxAbsolute;
    const auto operand    = reinterpret_cast<DWORD>(base + tmloader::abi::kWindowedFlag);
    std::memcpy(site + 1, &operand, sizeof(operand));
}

// Installs the hooks into a fresh image and returns it; the hooks land in `imports`.
auto installHooks() -> BYTE*
{
    keyboardHooks  = 0;
    parameterCalls = 0;
    lastParameter  = 0;
    imports.clear();
    auto* const base = allocateImage();
    writeCursorSync(base);
    installDesktopHooks(reinterpret_cast<HMODULE>(base));
    return base;
}

} // namespace

TEST_CASE("an unrecognized cursor sync layout is left unpatched", "[desktop]")
{
    auto* const base = allocateImage();

    REQUIRE_THROWS_AS(skipLegacyCursorSync(reinterpret_cast<HMODULE>(base)), std::runtime_error);
    REQUIRE(base[kSyncSiteRva] == 0);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("the legacy cursor sync is jumped over", "[desktop]")
{
    auto* const base = installHooks();
    BYTE* const site = base + kSyncSiteRva;

    auto displacement = LONG{};
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    REQUIRE(site[0] == kNearJump);
    REQUIRE(site + kNearJumpLength + displacement == base + kSyncResumeRva);
    REQUIRE(site[-1] == kBreakpoint);
    REQUIRE(site[kNearJumpLength] == kBreakpoint);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("seven imports are replaced", "[desktop]")
{
    auto* const base = installHooks();

    REQUIRE(imports.size() == 7);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("the global keyboard hook is refused and other hooks pass through", "[desktop]")
{
    auto* const base    = installHooks();
    const auto  keyHook = reinterpret_cast<decltype(&SetWindowsHookExA)>(imports.at(kKeyboardHookRva));

    REQUIRE(keyHook(WH_KEYBOARD_LL, nullptr, nullptr, 0) == nullptr);
    REQUIRE(keyboardHooks == 0);
    REQUIRE(keyHook(WH_GETMESSAGE, nullptr, nullptr, 123) != nullptr);
    REQUIRE(keyboardHooks == 1);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("system parameter writes are swallowed and reads pass through", "[desktop]")
{
    auto* const base       = installHooks();
    const auto  parameters = reinterpret_cast<decltype(&SystemParametersInfoA)>(imports.at(kParametersRva));

    for (const UINT action : { SPI_SETANIMATION, SPI_SETFOREGROUNDLOCKTIMEOUT, SPI_SETSCREENSAVEACTIVE, SPI_ICONHORIZONTALSPACING })
    {
        CAPTURE(action);
        REQUIRE(parameters(action, 0, nullptr, SPIF_SENDCHANGE));
    }

    REQUIRE(parameterCalls == 0);

    DWORD output = 0;
    REQUIRE(parameters(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &output, 0));
    REQUIRE(parameterCalls == 1);
    REQUIRE(lastParameter == SPI_GETFOREGROUNDLOCKTIMEOUT);
    REQUIRE(parameters(SPI_ICONHORIZONTALSPACING, 0, &output, 0));
    REQUIRE(parameterCalls == 2);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("ShowCursor reports a counter that crosses zero", "[desktop]")
{
    auto* const base   = installHooks();
    const auto  cursor = reinterpret_cast<decltype(&ShowCursor)>(imports.at(kShowCursorRva));

    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(cursor(FALSE) < 0);
        REQUIRE(cursor(TRUE) >= 0);
    }

    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("cursor warps and focus grabs succeed without acting", "[desktop]")
{
    auto* const base = installHooks();

    const auto warp = reinterpret_cast<decltype(&SetCursorPos)>(imports.at(kCursorPosRva));
    REQUIRE(warp(-9999, -9999));
    const auto attach = reinterpret_cast<decltype(&AttachThreadInput)>(imports.at(kAttachInputRva));
    REQUIRE(attach(0, 0, TRUE));
    const auto foreground = reinterpret_cast<decltype(&SetForegroundWindow)>(imports.at(kForegroundRva));
    REQUIRE(foreground(nullptr));
    const auto active = reinterpret_cast<decltype(&SetActiveWindow)>(imports.at(kActiveWindowRva));
    const auto window = reinterpret_cast<HWND>(1);
    REQUIRE(active(window) == window);
    VirtualFree(base, 0, MEM_RELEASE);
}
