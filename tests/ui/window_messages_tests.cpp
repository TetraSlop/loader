#include "app/runtime.h"
#include "ui/window.h"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <mutex>

// Exercise the actual subclass without creating or controlling any window.
namespace
{

const HWND        kTestWindow      = reinterpret_cast<HWND>(1);
const HWND        kOtherWindow     = reinterpret_cast<HWND>(2);
const HCURSOR     kArrowCursor     = reinterpret_cast<HCURSOR>(1);
const HCURSOR     kUntouchedCursor = reinterpret_cast<HCURSOR>(99);
const WNDPROC     kGameProcedure   = reinterpret_cast<WNDPROC>(1234);
constexpr POINT   kInsideGame      = { 320, 240 };
constexpr POINT   kLeftOfGame      = { -1, 240 };
constexpr UINT    kReclaimFocus    = 0x64;
constexpr LPARAM  kKeyRepeat       = 1L << 30;
constexpr LRESULT kDefaultResult   = 123;
constexpr LRESULT kLegacyResult    = 321;
// Index of the 4.5x preset in kRenderScales.
constexpr LPARAM kScaleIndex = 6;
constexpr float  kScale      = 4.5f;

UINT          defaultMessage;
WPARAM        defaultCommand;
int           quitRequests;
int           panelToggles;
int           panelStops;
int           legacyMessages;
int           screenshotRequests;
HWND          testForeground = kTestWindow;
bool          testPanelOpen;
POINT         testPointer = kInsideGame;
HCURSOR       shownCursor = kUntouchedCursor;
LONG_PTR      installedProcedure;
volatile bool forbidSubclass = true;

auto fakeSetCursor(HCURSOR cursor) -> HCURSOR
{
    shownCursor = cursor;
    return nullptr;
}

auto fakeLoadCursor(HINSTANCE, LPCWSTR) -> HCURSOR
{
    return kArrowCursor;
}

auto fakeForeground() -> HWND
{
    return testForeground;
}

auto fakeCursorPos(POINT* point) -> BOOL
{
    *point = testPointer;
    return TRUE;
}

auto fakeToClient(HWND, POINT*) -> BOOL
{
    return TRUE;
}

auto fakeClientRect(HWND, RECT* rect) -> BOOL
{
    *rect = { 0, 0, 640, 480 };
    return TRUE;
}

void fakeQuit(int)
{
    ++quitRequests;
}

auto fakeDefault(HWND, UINT message, WPARAM command, LPARAM) -> LRESULT
{
    defaultMessage = message;
    defaultCommand = command;
    return kDefaultResult;
}

auto fakeLegacy(WNDPROC, HWND, UINT, WPARAM, LPARAM) -> LRESULT
{
    ++legacyMessages;
    return kLegacyResult;
}

auto fakeGetWindowLongPtr(HWND hwnd, int index) -> LONG_PTR
{
    REQUIRE(hwnd == kTestWindow);
    REQUIRE(index == GWLP_WNDPROC);
    return installedProcedure;
}

auto fakeSetWindowLongPtr(HWND, int, LONG_PTR) -> LONG_PTR
{
    if (forbidSubclass)
    {
        throw std::runtime_error("Repeated initialization tried to subclass the window again");
    }

    return installedProcedure;
}

} // namespace

#undef GetWindowLongPtrA
#undef SetWindowLongPtrA
#define GetWindowLongPtrA   fakeGetWindowLongPtr
#define SetWindowLongPtrA   fakeSetWindowLongPtr
#define PostQuitMessage     fakeQuit
#define DefWindowProcA      fakeDefault
#define CallWindowProcA     fakeLegacy
#define SetCursor           fakeSetCursor
#define LoadCursorW         fakeLoadCursor
#define GetForegroundWindow fakeForeground
#define GetCursorPos        fakeCursorPos
#define ScreenToClient      fakeToClient
#define GetClientRect       fakeClientRect
#include "ui/window.cpp"
#undef PostQuitMessage
#undef DefWindowProcA
#undef CallWindowProcA

namespace tmloader
{

Config options;
HWND   gameWindow{};

void writeLog(std::string_view)
{
}

void toggleSettingsPanel()
{
    ++panelToggles;
}

void stopSettingsPanel()
{
    ++panelStops;
}

auto settingsPanelOpen() -> bool
{
    return testPanelOpen;
}

void requestScreenshot() noexcept
{
    ++screenshotRequests;
}

} // namespace tmloader

using namespace tmloader;

namespace
{

void resetFakes()
{
    defaultMessage     = 0;
    defaultCommand     = 0;
    quitRequests       = 0;
    panelToggles       = 0;
    panelStops         = 0;
    legacyMessages     = 0;
    screenshotRequests = 0;
    testForeground     = kTestWindow;
    testPanelOpen      = false;
    testPointer        = kInsideGame;
    shownCursor        = kUntouchedCursor;
}

// Points the loader at a runtime directory of its own, so cases can run in parallel.
void useRuntime(const char* name)
{
    resetFakes();
    options = parseConfig({ "--runtime-dir", (std::filesystem::current_path() / name).string() });
    REQUIRE_FALSE(std::filesystem::exists(options.runtimeDir));
}

void sendSetting(WindowAction action, LPARAM value)
{
    gameWindowProc(nullptr, kSettingsMessage, static_cast<WPARAM>(action), value);
}

auto sendSetCursor(WORD hitTest) -> LRESULT
{
    return gameWindowProc(kTestWindow, WM_SETCURSOR, reinterpret_cast<WPARAM>(kTestWindow), MAKELPARAM(hitTest, WM_MOUSEMOVE));
}

} // namespace

TEST_CASE("move and size commands reach the default window procedure", "[window_messages]")
{
    resetFakes();
    for (const WPARAM command : { SC_SIZE, SC_MOVE, SC_MINIMIZE, SC_MAXIMIZE, SC_RESTORE })
    {
        CAPTURE(command);
        REQUIRE(gameWindowProc(nullptr, WM_SYSCOMMAND, command, 0) == kDefaultResult);
        REQUIRE(defaultMessage == WM_SYSCOMMAND);
        REQUIRE(defaultCommand == command);
    }

    REQUIRE(legacyMessages == 0);
}

TEST_CASE("the focus reclaim message is dropped", "[window_messages]")
{
    resetFakes();
    // A lost-focus notification must not reach the native restore/foreground path.
    gameWindowProc(kTestWindow, kReclaimFocus, 0, 0);
    REQUIRE(legacyMessages == 0);
}

TEST_CASE("the system cursor is hidden only over the focused game", "[window_messages]")
{
    resetFakes();
    // The game draws a software cursor there.
    REQUIRE(sendSetCursor(HTCLIENT) == TRUE);
    REQUIRE(shownCursor == nullptr);

    testPanelOpen = true;
    REQUIRE(sendSetCursor(HTCLIENT) == TRUE);
    REQUIRE(shownCursor != nullptr);

    testPanelOpen  = false;
    testForeground = kOtherWindow;
    REQUIRE(sendSetCursor(HTCLIENT) == TRUE);
    REQUIRE(shownCursor != nullptr);

    testForeground = kTestWindow;
    testPointer    = kLeftOfGame;
    REQUIRE(sendSetCursor(HTCLIENT) == TRUE);
    REQUIRE(shownCursor != nullptr);
}

TEST_CASE("Windows still chooses the resize cursors", "[window_messages]")
{
    resetFakes();
    REQUIRE(sendSetCursor(HTLEFT) == kDefaultResult);
}

TEST_CASE("every way of closing the window stops the panel and quits", "[window_messages]")
{
    useRuntime("window-close-fixture");

    gameWindowProc(nullptr, WM_SYSCOMMAND, SC_CLOSE | 2, 0);
    REQUIRE(quitRequests == 1);
    REQUIRE(panelStops == 1);

    gameWindowProc(nullptr, WM_CLOSE, 0, 0);
    REQUIRE(quitRequests == 2);
    REQUIRE(panelStops == 2);

    gameWindowProc(nullptr, WM_SYSKEYDOWN, VK_F4, 0);
    REQUIRE(quitRequests == 3);
    REQUIRE(panelStops == 3);
    std::filesystem::remove_all(options.runtimeDir);
}

TEST_CASE("F10 toggles the settings panel once per press", "[window_messages]")
{
    resetFakes();
    gameWindowProc(nullptr, WM_KEYDOWN, VK_F10, 0);
    gameWindowProc(nullptr, WM_KEYDOWN, VK_F10, kKeyRepeat);
    gameWindowProc(nullptr, WM_SYSKEYUP, VK_F10, 0);
    REQUIRE(panelToggles == 1);
}

TEST_CASE("F12 requests one screenshot per press and stays out of the game", "[window_messages]")
{
    resetFakes();
    gameWindowProc(nullptr, WM_KEYDOWN, VK_F12, 0);
    gameWindowProc(nullptr, WM_KEYDOWN, VK_F12, kKeyRepeat);
    gameWindowProc(nullptr, WM_KEYUP, VK_F12, 0);
    REQUIRE(screenshotRequests == 1);
    REQUIRE(legacyMessages == 0);
}

TEST_CASE("other messages are forwarded to the game", "[window_messages]")
{
    resetFakes();
    gameWindowProc(nullptr, WM_MOUSEMOVE, 0, 0);
    REQUIRE(legacyMessages == 1);
}

TEST_CASE("a left click is latched until it is read", "[window_messages]")
{
    resetFakes();
    gameWindowProc(nullptr, WM_LBUTTONDOWN, 0, 0);
    REQUIRE(takeMousePresses() == LeftButton);
    REQUIRE(takeMousePresses() == 0);
}

TEST_CASE("losing focus discards a latched click", "[window_messages]")
{
    resetFakes();
    gameWindowProc(nullptr, WM_LBUTTONDOWN, 0, 0);
    gameWindowProc(nullptr, WM_KILLFOCUS, 0, 0);
    REQUIRE(takeMousePresses() == 0);
}

TEST_CASE("a render scale choice is applied and saved, and a bad index is ignored", "[window_messages]")
{
    useRuntime("window-scale-fixture");

    sendSetting(WindowAction::renderScale, kScaleIndex);
    REQUIRE(currentWindowSettings().renderScale == kScale);
    REQUIRE_FALSE(currentWindowSettings().autoResolution);

    sendSetting(WindowAction::renderScale, -1);
    sendSetting(WindowAction::renderScale, kRenderScales.size());
    REQUIRE(currentWindowSettings().renderScale == kScale);

    std::ifstream saved(settingsFile());
    REQUIRE(readWindowSettings(saved).renderScale == kScale);
    saved.close();
    std::filesystem::remove_all(options.runtimeDir);
}

TEST_CASE("automatic resolution is saved", "[window_messages]")
{
    useRuntime("window-toggle-fixture");
    sendSetting(WindowAction::renderScale, kScaleIndex);

    sendSetting(WindowAction::autoResolution, 0);
    REQUIRE(currentWindowSettings().autoResolution);
    REQUIRE(currentWindowSettings().renderScale == kScale);

    std::ifstream updated(settingsFile());
    const auto    preferences = readWindowSettings(updated);
    REQUIRE(preferences.autoResolution);
    updated.close();
    std::filesystem::remove_all(options.runtimeDir);
}

TEST_CASE("initialising an already subclassed window changes nothing", "[window_messages]")
{
    useRuntime("window-reinit-fixture");
    sendSetting(WindowAction::renderScale, kScaleIndex);

    // The fake SetWindowLongPtrA throws if the window is subclassed a second time.
    gameWindow         = kTestWindow;
    originalWindowProc = kGameProcedure;
    installedProcedure = reinterpret_cast<LONG_PTR>(gameWindowProc);
    initializeGameWindow(kTestWindow);
    REQUIRE(originalWindowProc == kGameProcedure);
    REQUIRE(currentWindowSettings().renderScale == kScale);
    std::filesystem::remove_all(options.runtimeDir);
}
