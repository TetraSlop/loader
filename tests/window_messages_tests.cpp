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
static void fakeQuit(int) { ++quitRequests; }
static LRESULT fakeDefault(HWND, UINT message, WPARAM command, LPARAM) {
    defaultMessage = message; defaultCommand = command; return 123;
}
static LRESULT fakeLegacy(WNDPROC, HWND, UINT, WPARAM, LPARAM) { ++legacyMessages; return 321; }
#define PostQuitMessage fakeQuit
#define DefWindowProcA fakeDefault
#define CallWindowProcA fakeLegacy
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
    std::filesystem::remove_all(options.runtimeDir);
}
