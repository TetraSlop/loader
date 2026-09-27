#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include "window_settings.h"
static const GUID keyboardGUID = {1};
static const GUID mouseGUID = {2};
static const IID inputIID = {3};
static void logf(const char *, ...) {}
static POINT testPointer = {};
static LONG testWidth = 640, testHeight = 480;
static bool testFocused = true;
static SHORT testButton = 0;
static LONG virtualX = 0, virtualY = 0;
static int mappedSamples = 0;
static BYTE latchedPresses = 0;
static BYTE fakePresses() { BYTE value=latchedPresses; latchedPresses=0; return value; }
static bool fakeMapper(LONG x, LONG y, LONG *dx, LONG *dy)
{
    *dx = x - virtualX; *dy = y - virtualY;
    virtualX = x; virtualY = y; ++mappedSamples;
    return true;
}
static HWND testWindow = reinterpret_cast<HWND>(1);
static HWND testForeground = testWindow;
static HWND fakeForeground() { return testForeground; }
static bool panelOpen = false;
static bool fakeBlocked() { return panelOpen; }
static bool fakeViewport(RECT* rect) {
    *rect = tmloader::contentRect(testWidth, testHeight, true);
    return true;
}
static DWORD fakeWindowThread(HWND, LPDWORD pid) { *pid = testFocused ? GetCurrentProcessId() : 0; return 1; }
static BOOL fakeCursor(LPPOINT p) { *p = testPointer; return TRUE; }
static BOOL fakeToClient(HWND, LPPOINT) { return TRUE; }
static BOOL fakeClientRect(HWND, LPRECT r) { *r = {0, 0, testWidth, testHeight}; return TRUE; }
static int pressedKey = 0;
static SHORT fakeKey(int key) { return key == VK_LBUTTON ? testButton : key == pressedKey ? static_cast<SHORT>(0x8000) : 0; }
#define GetForegroundWindow fakeForeground
#define GetWindowThreadProcessId fakeWindowThread
#define GetCursorPos fakeCursor
#define ScreenToClient fakeToClient
#define GetClientRect fakeClientRect
#define GetAsyncKeyState fakeKey
#include "input_bridge.h"
static void sample(LocalInputDevice &mouse, LONG x, LONG y, LONG dx, LONG dy)
{
    testPointer = {x, y};
    DIMOUSESTATE2 state = {};
    REQUIRE(mouse.GetDeviceState(sizeof(state), &state) == S_OK);
    REQUIRE((state.lX == dx && state.lY == dy));
}
TEST_CASE("Input bridge maps focused input and preserves cursor transitions", "[input_bridge]")
{
    LocalInputDevice mouse(false);
    mouse.SetCooperativeLevel(testWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(mouse, 200, 200, 0, 0);
    sample(mouse, 210, 195, 10, -5);
    testFocused = false;
    sample(mouse, 1800, 900, 0, 0);
    testFocused = true;
    sample(mouse, 50, 40, 0, 0);
    sample(mouse, 60, 45, 10, 5);
    sample(mouse, -20, 40, 0, 0);
    sample(mouse, 30, 50, 0, 0);
    sample(mouse, 40, 50, 10, 0);
    testWidth = 1280; testHeight = 960;
    sample(mouse, 80, 100, 0, 0);
    sample(mouse, 100, 110, 10, 5);
    mouse.Unacquire(); mouse.Acquire();
    sample(mouse, 900, 700, 0, 0);
    sample(mouse, 880, 680, -10, -10);
    localMouseMapper = fakeMapper;
    virtualX = 300; virtualY = 300;
    sample(mouse, 900, 700, 150, 50);
    // Keyboard/focus animation may move the game cursor while the physical
    // pointer stays still. Do not fight it until physical input takes over.
    virtualX = 600; virtualY = 400;
    sample(mouse, 900, 700, 0, 0);
    REQUIRE((mappedSamples == 1 && virtualX == 600));
    sample(mouse, 902, 702, -149, -49);
    virtualX = 100; virtualY = 100;
    testButton = static_cast<SHORT>(0x8000);
    sample(mouse, 902, 702, 351, 251);
    REQUIRE(mappedSamples == 3);
    sample(mouse, 902, 702, 0, 0);
    REQUIRE(mappedSamples == 3);
    testButton = 0;
    sample(mouse, 1279, 959, 188, 128);
    REQUIRE((virtualX == 639 && virtualY == 479));
    localMousePresses = fakePresses;
    latchedPresses = 1;
    DIMOUSESTATE2 state = {};
    mouse.GetDeviceState(sizeof(state), &state);
    REQUIRE(state.rgbButtons[0] == 0x80);
    mouse.GetDeviceState(sizeof(state), &state);
    REQUIRE(state.rgbButtons[0] == 0);
    // 16:9 client with 4:3 game content: bars do not click or shift the cursor.
    localContentRect = fakeViewport;
    localInputBlocked = fakeBlocked;
    testWidth = 1920; testHeight = 1080;
    virtualX = virtualY = 0;
    sample(mouse, 240, 0, 0, 0);
    sample(mouse, 960, 540, 320, 240);
    sample(mouse, 1679, 1079, 319, 239);
    latchedPresses = 1;
    sample(mouse, 100, 500, 0, 0);
    REQUIRE((latchedPresses == 0 && virtualX == 639));
    sample(mouse, 960, 540, -319, -239);
    // Our settings window shares the process, but must never feed the game.
    testForeground = reinterpret_cast<HWND>(2);
    sample(mouse, 1200, 800, 0, 0);
    testForeground = testWindow;
    panelOpen = true;
    latchedPresses = 1;
    sample(mouse, 1200, 800, 0, 0);
    REQUIRE(latchedPresses == 0);
    panelOpen = false;
    sample(mouse, 960, 540, 0, 0);
    LocalInputDevice keyboard(true);
    keyboard.SetCooperativeLevel(testWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    BYTE keys[256]{};
    for (int key : {VK_F10, VK_F12}) {
        pressedKey = key;
        REQUIRE(keyboard.GetDeviceState(sizeof(keys), keys) == S_OK);
        for (BYTE value : keys) REQUIRE(value == 0);
    }
    pressedKey = 'A';
    REQUIRE((keyboard.GetDeviceState(sizeof(keys), keys) == S_OK && keys[DIK_A] == 0x80));
    puts("Mouse bridge: transitions, scaling, absolute alignment, keyboard handoff and clicks passed.");
}
