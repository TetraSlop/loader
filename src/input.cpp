#include "runtime.h"
#include "window.h"
namespace tmloader {
static constexpr IID inputIID = {0xbf798030, 0x483a, 0x4da2, {0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00}};
static constexpr GUID keyboardGUID = {0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static constexpr GUID mouseGUID = {0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
#include "input_bridge.h"
static bool inputContentRect(RECT* rect) {
    *rect = gameContentRect();
    return rect->right > rect->left && rect->bottom > rect->top;
}
static bool mapTetraMouse(LONG x, LONG y, LONG *dx, LONG *dy)
{
    if (!gameBase || !gameWindow)
        return false;
    // TM 1.6.11.0: input object102FF570; signed cursor words+2C/+2E.
    // 10189270 chooses animation(+14) or integrates the relative axes.
    // Cancel only for physical mouse movement/click, retaining keyboard focus
    // animation while the pointer is stationary.
    auto* const input = gameBase + 0x2ff570;
    *reinterpret_cast<LONG*>(input + 0x14) = 0;
    *reinterpret_cast<LONG*>(input + 0x18) = 0;
    *dx = x - *reinterpret_cast<const SHORT*>(input + 0x2c);
    *dy = y - *reinterpret_cast<const SHORT*>(input + 0x2e);
    return true;
}
HRESULT WINAPI tracedInput(HINSTANCE instance, DWORD version, REFIID iid, void **output, IUnknown *outer)
{
    using Factory = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const auto real = reinterpret_cast<Factory>(GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create"));
    const auto hr = real(instance, version, iid, output, outer);
    logf("DirectInput8Create instance=%p version=%lX IID=%08lX hr=%08lX obj=%p", instance, version, iid.Data1, hr,
         *output);
    if (FAILED(hr) && iid == inputIID && !outer)
    {
        localMouseMapper = mapTetraMouse;
        localMousePresses = takeMousePresses;
        localInputBlocked = settingsPanelOpen;
        localContentRect = inputContentRect;
        *output = new LocalInput();
        logf("Using local keyboard/mouse bridge");
        return S_OK;
    }
    return hr;
}
}
