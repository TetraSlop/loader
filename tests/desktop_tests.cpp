#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime.h"
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
static int keyboardHooks, parameterCalls;
static UINT lastParameter;
static HHOOK fakeKeyboardHook(int, HOOKPROC, HINSTANCE, DWORD) {
    ++keyboardHooks;
    return reinterpret_cast<HHOOK>(1);
}
static BOOL fakeParameters(UINT action, UINT, PVOID, UINT) {
    ++parameterCalls;
    lastParameter = action;
    return TRUE;
}
#define SetWindowsHookExA fakeKeyboardHook
#define SystemParametersInfoA fakeParameters
#include "../src/desktop.cpp"
#undef SetWindowsHookExA
#undef SystemParametersInfoA
namespace tmloader {
static std::map<DWORD, void*> imports;
void logf(const char*, ...) {}
void setSlot(HMODULE, DWORD rva, void* function) { imports[rva] = function; }
}
int main() {
    using namespace tmloader;
    // In-memory fixture only: no game DLL, keyboard hook or desktop mutation.
    auto* base = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x310000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(base);
    auto module = reinterpret_cast<HMODULE>(base);
    bool rejected = false;
    try { skipLegacyCursorSync(module); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && base[0x185753] == 0);
    BYTE* site = base + 0x185753;
    site[-1] = 0xcc; site[5] = 0xcc;
    site[0] = 0xa1;
    DWORD operand = reinterpret_cast<DWORD>(base + 0x2ff248);
    std::memcpy(site + 1, &operand, sizeof(operand));
    installDesktopHooks(module);
    LONG displacement;
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    assert(site[0] == 0xe9 && site + 5 + displacement == base + 0x185830);
    assert(site[-1] == 0xcc && site[5] == 0xcc && imports.size() == 7);

    auto keyHook = reinterpret_cast<decltype(&SetWindowsHookExA)>(imports.at(0x2041e4));
    assert(keyHook(WH_KEYBOARD_LL, nullptr, nullptr, 0) == nullptr && keyboardHooks == 0);
    assert(keyHook(WH_GETMESSAGE, nullptr, nullptr, 123) != nullptr && keyboardHooks == 1);
    auto parameters = reinterpret_cast<decltype(&SystemParametersInfoA)>(imports.at(0x2041e8));
    for (UINT action : {SPI_SETANIMATION, SPI_SETFOREGROUNDLOCKTIMEOUT, SPI_SETSCREENSAVEACTIVE, SPI_ICONHORIZONTALSPACING})
        assert(parameters(action, 0, nullptr, SPIF_SENDCHANGE));
    assert(parameterCalls == 0);
    DWORD output = 0;
    assert(parameters(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &output, 0));
    assert(parameterCalls == 1 && lastParameter == SPI_GETFOREGROUNDLOCKTIMEOUT);
    assert(parameters(SPI_ICONHORIZONTALSPACING, 0, &output, 0) && parameterCalls == 2);

    auto cursor = reinterpret_cast<decltype(&ShowCursor)>(imports.at(0x204248));
    for (int i = 0; i < 10; ++i) assert(cursor(FALSE) < 0 && cursor(TRUE) >= 0);
    auto warp = reinterpret_cast<decltype(&SetCursorPos)>(imports.at(0x2041f0));
    assert(warp(-9999, -9999));
    auto attach = reinterpret_cast<decltype(&AttachThreadInput)>(imports.at(0x204240));
    assert(attach(0, 0, TRUE));
    auto foreground = reinterpret_cast<decltype(&SetForegroundWindow)>(imports.at(0x204244));
    assert(foreground(nullptr));
    auto active = reinterpret_cast<decltype(&SetActiveWindow)>(imports.at(0x204258));
    HWND window = reinterpret_cast<HWND>(1);
    assert(active(window) == window);
    VirtualFree(base, 0, MEM_RELEASE);
}
