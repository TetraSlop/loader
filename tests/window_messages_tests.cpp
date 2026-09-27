#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime.h"
#include "window.h"
#include <cassert>
#include <fstream>
#include <mutex>

// Exercise the actual subclass without creating or controlling any window.
static UINT defaultMessage;
static WPARAM defaultCommand;
static int quitRequests, panelToggles, panelStops, legacyMessages;
static HWND testWindow = reinterpret_cast<HWND>(1);
static HWND testForeground = testWindow;
static bool testPanelOpen;
static POINT testPointer{320, 240};
static HCURSOR shownCursor = reinterpret_cast<HCURSOR>(99);
static HCURSOR fakeSetCursor(HCURSOR cursor) { shownCursor = cursor; return nullptr; }
static HCURSOR fakeLoadCursor(HINSTANCE, LPCWSTR) { return reinterpret_cast<HCURSOR>(1); }
static HWND fakeForeground() { return testForeground; }
static BOOL fakeCursorPos(POINT* point) { *point = testPointer; return TRUE; }
static BOOL fakeToClient(HWND, POINT*) { return TRUE; }
static BOOL fakeClientRect(HWND, RECT* rect) { *rect = {0, 0, 640, 480}; return TRUE; }
static void fakeQuit(int) { ++quitRequests; }
static LRESULT fakeDefault(HWND, UINT message, WPARAM command, LPARAM) {
    defaultMessage = message; defaultCommand = command; return 123;
}
static LRESULT fakeLegacy(WNDPROC, HWND, UINT, WPARAM, LPARAM) { ++legacyMessages; return 321; }
static LONG_PTR installedProcedure;
static volatile bool forbidSubclass = true;
static LONG_PTR fakeGetWindowLongPtr(HWND hwnd, int index) {
    assert(hwnd == testWindow && index == GWLP_WNDPROC);
    return installedProcedure;
}
static LONG_PTR fakeSetWindowLongPtr(HWND, int, LONG_PTR) {
    if (forbidSubclass) throw std::runtime_error("Repeated initialization tried to subclass the window again");
    return installedProcedure;
}
#undef GetWindowLongPtrA
#undef SetWindowLongPtrA
#define GetWindowLongPtrA fakeGetWindowLongPtr
#define SetWindowLongPtrA fakeSetWindowLongPtr
#define PostQuitMessage fakeQuit
#define DefWindowProcA fakeDefault
#define CallWindowProcA fakeLegacy
#define SetCursor fakeSetCursor
#define LoadCursorW fakeLoadCursor
#define GetForegroundWindow fakeForeground
#define GetCursorPos fakeCursorPos
#define ScreenToClient fakeToClient
#define GetClientRect fakeClientRect
#include "../src/window.cpp"
#undef PostQuitMessage
#undef DefWindowProcA
#undef CallWindowProcA

namespace tmloader {
Config options;
HWND gameWindow{};
void logf(const char*, ...) {}
void toggleSettingsPanel() { ++panelToggles; }
void stopSettingsPanel() { ++panelStops; }
bool settingsPanelOpen() { return testPanelOpen; }
}
int main() {
    using namespace tmloader;
    options = parseConfig({"--runtime-dir", (std::filesystem::current_path() / "window-message-fixture").string()});
    assert(!std::filesystem::exists(options.runtimeDir));
    for (WPARAM command : {SC_SIZE, SC_MOVE, SC_MINIMIZE, SC_MAXIMIZE, SC_RESTORE}) {
        assert(wrapperWindow(nullptr, WM_SYSCOMMAND, command, 0) == 123);
        assert(defaultMessage == WM_SYSCOMMAND && defaultCommand == command);
    }
    assert(legacyMessages == 0);
    // A lost-focus notification must not reach the native restore/foreground path.
    wrapperWindow(testWindow, 0x64, 0, 0);
    assert(legacyMessages == 0);
    auto cursor = [&] { return wrapperWindow(testWindow, WM_SETCURSOR,
        reinterpret_cast<WPARAM>(testWindow), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)); };
    assert(cursor() == TRUE && shownCursor == nullptr); // Software cursor inside game.
    testPanelOpen = true;
    assert(cursor() == TRUE && shownCursor != nullptr);
    testPanelOpen = false;
    testForeground = reinterpret_cast<HWND>(2);
    assert(cursor() == TRUE && shownCursor != nullptr);
    testForeground = testWindow;
    testPointer = {-1, 240};
    assert(cursor() == TRUE && shownCursor != nullptr);
    assert(wrapperWindow(testWindow, WM_SETCURSOR, reinterpret_cast<WPARAM>(testWindow),
        MAKELPARAM(HTLEFT, WM_MOUSEMOVE)) == 123); // Windows still chooses resize arrows.
    wrapperWindow(nullptr, WM_SYSCOMMAND, SC_CLOSE | 2, 0);
    assert(quitRequests == 1 && panelStops == 1);
    wrapperWindow(nullptr, WM_CLOSE, 0, 0);
    assert(quitRequests == 2 && panelStops == 2);
    wrapperWindow(nullptr, WM_SYSKEYDOWN, VK_F4, 0);
    assert(quitRequests == 3 && panelStops == 3);
    wrapperWindow(nullptr, WM_KEYDOWN, VK_F10, 0);
    wrapperWindow(nullptr, WM_KEYDOWN, VK_F10, 1L << 30);
    wrapperWindow(nullptr, WM_SYSKEYUP, VK_F10, 0);
    assert(panelToggles == 1);
    wrapperWindow(nullptr, WM_MOUSEMOVE, 0, 0);
    assert(legacyMessages == 1);
    wrapperWindow(nullptr, WM_LBUTTONDOWN, 0, 0);
    assert(takeMousePresses() == 1 && takeMousePresses() == 0);
    wrapperWindow(nullptr, WM_LBUTTONDOWN, 0, 0);
    wrapperWindow(nullptr, WM_KILLFOCUS, 0, 0);
    assert(takeMousePresses() == 0);
    wrapperWindow(nullptr, settingsMessage, static_cast<WPARAM>(WindowAction::renderScale), 4);
    assert(currentWindowSettings().renderScale == 4.5f);
    wrapperWindow(nullptr, settingsMessage, static_cast<WPARAM>(WindowAction::renderScale), -1);
    wrapperWindow(nullptr, settingsMessage, static_cast<WPARAM>(WindowAction::renderScale), renderScales.size());
    assert(currentWindowSettings().renderScale == 4.5f);
    std::ifstream saved(settingsFile());
    assert(readWindowSettings(saved).renderScale == 4.5f);
    saved.close();
    gameWindow = testWindow;
    originalWindowProc = reinterpret_cast<WNDPROC>(1234);
    installedProcedure = reinterpret_cast<LONG_PTR>(wrapperWindow);
    initializeGameWindow(testWindow);
    assert(originalWindowProc == reinterpret_cast<WNDPROC>(1234));
    assert(currentWindowSettings().renderScale == 4.5f);
    std::filesystem::remove_all(options.runtimeDir);
}
