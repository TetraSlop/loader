#include <windows.h>

#include "ui/window_settings.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <dinput.h>
#include <utility>

namespace
{

const HWND      kTestWindow  = reinterpret_cast<HWND>(1);
const HWND      kOtherWindow = reinterpret_cast<HWND>(2);
constexpr SHORT kKeyDown     = static_cast<SHORT>(0x8000);

POINT testPointer    = {};
LONG  testWidth      = 640;
LONG  testHeight     = 480;
bool  testFocused    = true;
SHORT testButton     = 0;
LONG  virtualX       = 0;
LONG  virtualY       = 0;
int   mappedSamples  = 0;
BYTE  latchedPresses = 0;
HWND  testForeground = kTestWindow;
bool  panelOpen      = false;
bool  mapperEnabled  = false;

auto fakeForeground() -> HWND
{
    return testForeground;
}

auto fakeWindowThread(HWND, LPDWORD pid) -> DWORD
{
    *pid = 0;
    if (testFocused)
    {
        *pid = GetCurrentProcessId();
    }

    return 1;
}

auto fakeCursor(LPPOINT p) -> BOOL
{
    *p = testPointer;
    return TRUE;
}

auto fakeToClient(HWND, LPPOINT) -> BOOL
{
    return TRUE;
}

auto fakeKey(int key) -> SHORT
{
    if (key == VK_LBUTTON)
    {
        return testButton;
    }

    return 0;
}

} // namespace

namespace tmloader
{

void writeLog(std::string_view)
{
}

auto takeMousePresses() -> BYTE
{
    return std::exchange(latchedPresses, BYTE{ 0 });
}

auto mapGameMouse(LONG x, LONG y, LONG* dx, LONG* dy) -> bool
{
    if (!mapperEnabled)
    {
        return false;
    }

    *dx      = x - virtualX;
    *dy      = y - virtualY;
    virtualX = x;
    virtualY = y;
    ++mappedSamples;
    return true;
}

auto settingsPanelOpen() -> bool
{
    return panelOpen;
}

auto gameContentRect() -> RECT
{
    return contentRect(testWidth, testHeight, true);
}

} // namespace tmloader

#define GetForegroundWindow      fakeForeground
#define GetWindowThreadProcessId fakeWindowThread
#define GetCursorPos             fakeCursor
#define ScreenToClient           fakeToClient
#define GetAsyncKeyState         fakeKey
#include "input/input_bridge.cpp"
using namespace tmloader;

namespace
{

// A focused game window with the given client size, the pointer at the origin and no cursor mapper.
void resetDesktop(LONG width, LONG height)
{
    testPointer    = {};
    testWidth      = width;
    testHeight     = height;
    testFocused    = true;
    testButton     = 0;
    virtualX       = 0;
    virtualY       = 0;
    mappedSamples  = 0;
    latchedPresses = 0;
    testForeground = kTestWindow;
    panelOpen      = false;
    mapperEnabled  = false;
}

void enableMapper(LONG x, LONG y)
{
    mapperEnabled = true;
    virtualX      = x;
    virtualY      = y;
}

// Moves the pointer to (x, y) and expects the mouse to report (dx, dy).
void sample(LocalMouse& mouse, LONG x, LONG y, LONG dx, LONG dy)
{
    testPointer = { x, y };
    auto state  = DIMOUSESTATE2{};
    REQUIRE(mouse.GetDeviceState(sizeof(state), &state) == S_OK);
    REQUIRE(state.lX == dx);
    REQUIRE(state.lY == dy);
}

} // namespace

TEST_CASE("pointer movement is reported as relative motion", "[input_bridge]")
{
    resetDesktop(640, 480);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);

    sample(*mouse.Get(), 200, 200, 0, 0);
    sample(*mouse.Get(), 210, 195, 10, -5);
}

TEST_CASE("regaining focus does not report the distance travelled meanwhile", "[input_bridge]")
{
    resetDesktop(640, 480);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 210, 195, 0, 0);

    testFocused = false;
    sample(*mouse.Get(), 1800, 900, 0, 0);
    testFocused = true;
    sample(*mouse.Get(), 50, 40, 0, 0);
    sample(*mouse.Get(), 60, 45, 10, 5);
}

TEST_CASE("re-entering the client area does not report a jump", "[input_bridge]")
{
    resetDesktop(640, 480);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 60, 45, 0, 0);

    sample(*mouse.Get(), -20, 40, 0, 0);
    sample(*mouse.Get(), 30, 50, 0, 0);
    sample(*mouse.Get(), 40, 50, 10, 0);
}

TEST_CASE("motion is scaled to 640x480 after the window is resized", "[input_bridge]")
{
    resetDesktop(640, 480);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 40, 50, 0, 0);

    testWidth  = 1280;
    testHeight = 960;
    sample(*mouse.Get(), 80, 100, 0, 0);
    sample(*mouse.Get(), 100, 110, 10, 5);
}

TEST_CASE("reacquiring the device does not report a jump", "[input_bridge]")
{
    resetDesktop(1280, 960);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 100, 110, 0, 0);

    mouse->Unacquire();
    mouse->Acquire();
    sample(*mouse.Get(), 900, 700, 0, 0);
    sample(*mouse.Get(), 880, 680, -10, -10);
}

TEST_CASE("the game cursor follows the pointer only while the pointer moves", "[input_bridge]")
{
    resetDesktop(1280, 960);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 880, 680, 0, 0);

    enableMapper(300, 300);
    sample(*mouse.Get(), 900, 700, 150, 50);
    // Keyboard/focus animation may move the game cursor while the physical
    // pointer stays still. Do not fight it until physical input takes over.
    virtualX = 600;
    virtualY = 400;
    sample(*mouse.Get(), 900, 700, 0, 0);
    REQUIRE(mappedSamples == 1);
    REQUIRE(virtualX == 600);
    sample(*mouse.Get(), 902, 702, -149, -49);
}

TEST_CASE("a click moves the game cursor to a still pointer once", "[input_bridge]")
{
    resetDesktop(1280, 960);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 902, 702, 0, 0);

    enableMapper(100, 100);
    testButton = kKeyDown;
    sample(*mouse.Get(), 902, 702, 351, 251);
    REQUIRE(mappedSamples == 1);
    sample(*mouse.Get(), 902, 702, 0, 0);
    REQUIRE(mappedSamples == 1);
}

TEST_CASE("the game cursor stops at the last game pixel", "[input_bridge]")
{
    resetDesktop(1280, 960);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 902, 702, 0, 0);

    enableMapper(451, 351);
    sample(*mouse.Get(), 1279, 959, 188, 128);
    REQUIRE(virtualX == 639);
    REQUIRE(virtualY == 479);
}

TEST_CASE("a press latched between polls is reported once", "[input_bridge]")
{
    resetDesktop(640, 480);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 200, 200, 0, 0);

    latchedPresses = 1;
    auto state     = DIMOUSESTATE2{};
    mouse->GetDeviceState(sizeof(state), &state);
    REQUIRE(state.rgbButtons[0] == 0x80);
    mouse->GetDeviceState(sizeof(state), &state);
    REQUIRE(state.rgbButtons[0] == 0);
}

TEST_CASE("pillarbox bars neither click nor move the game cursor", "[input_bridge]")
{
    // 16:9 client with 4:3 game content.
    resetDesktop(1920, 1080);
    enableMapper(0, 0);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);

    sample(*mouse.Get(), 240, 0, 0, 0);
    sample(*mouse.Get(), 960, 540, 320, 240);
    sample(*mouse.Get(), 1679, 1079, 319, 239);
    latchedPresses = 1;
    sample(*mouse.Get(), 100, 500, 0, 0);
    REQUIRE(latchedPresses == 0);
    REQUIRE(virtualX == 639);
    sample(*mouse.Get(), 960, 540, -319, -239);
}

TEST_CASE("the settings window never feeds the game", "[input_bridge]")
{
    resetDesktop(1920, 1080);
    enableMapper(320, 240);
    const auto mouse = Microsoft::WRL::Make<LocalMouse>();
    REQUIRE(mouse);
    mouse->SetCooperativeLevel(kTestWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    sample(*mouse.Get(), 960, 540, 0, 0);

    // It shares the process, so the foreground window has to be compared as well.
    testForeground = kOtherWindow;
    sample(*mouse.Get(), 1200, 800, 0, 0);
    testForeground = kTestWindow;
    panelOpen      = true;
    latchedPresses = 1;
    sample(*mouse.Get(), 1200, 800, 0, 0);
    REQUIRE(latchedPresses == 0);
    panelOpen = false;
    sample(*mouse.Get(), 960, 540, 0, 0);
}

TEST_CASE("the input objects answer the interfaces the game asks for", "[input_bridge]")
{
    REQUIRE(__uuidof(IDirectInput8A) == IID_IDirectInput8A);
    REQUIRE(__uuidof(IDirectInputDevice8A) == IID_IDirectInputDevice8A);

    const Microsoft::WRL::ComPtr<IDirectInput8A> input = Microsoft::WRL::Make<LocalInput>();
    REQUIRE(input);
    void* out = nullptr;
    REQUIRE(input->QueryInterface(IID_IDirectInput8A, &out) == S_OK);
    REQUIRE(out == input.Get());
    REQUIRE(input.Get()->Release() == 1);
    REQUIRE(input->QueryInterface(IID_IDirectInput8W, &out) == E_NOINTERFACE);
    REQUIRE(out == nullptr);

    IDirectInputDevice8A* device = nullptr;
    REQUIRE(input->CreateDevice(GUID_SysKeyboard, &device, nullptr) == DIERR_DEVICENOTREG);
    REQUIRE(device == nullptr);
    REQUIRE(input->CreateDevice(GUID_SysMouse, &device, nullptr) == S_OK);
    REQUIRE(device != nullptr);

    // The mouse answers any interface ID with itself.
    REQUIRE(device->QueryInterface(IID_IDirectInput8W, &out) == S_OK);
    REQUIRE(out == device);
    REQUIRE(device->Release() == 1);
    REQUIRE(device->Release() == 0);
}
