#include "app/runtime.h"
#include "input/input_bridge.h"
#include "ui/window.h"

namespace tmloader
{
namespace
{

auto inputContentRect(RECT* rect) -> bool
{
    *rect = gameContentRect();
    return rect->right > rect->left && rect->bottom > rect->top;
}

auto mapTetraMouse(LONG x, LONG y, LONG* dx, LONG* dy) -> bool
{
    if (!gameBase || !gameWindow)
    {
        return false;
    }
    // TM 1.6.11.0: input object102FF570; signed cursor words+2C/+2E.
    // 10189270 chooses animation(+14) or integrates the relative axes.
    // Cancel only for physical mouse movement/click, retaining keyboard focus
    // animation while the pointer is stationary.
    auto* const input                      = gameBase + 0x2ff570;
    *reinterpret_cast<LONG*>(input + 0x14) = 0;
    *reinterpret_cast<LONG*>(input + 0x18) = 0;
    *dx                                    = x - *reinterpret_cast<const SHORT*>(input + 0x2c);
    *dy                                    = y - *reinterpret_cast<const SHORT*>(input + 0x2e);
    return true;
}

} // namespace

auto WINAPI tracedInput(HINSTANCE instance, DWORD version, REFIID iid, void** output, IUnknown* outer) -> HRESULT
{
    using Factory   = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const auto real = reinterpret_cast<Factory>(GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create"));
    const auto hr   = real(instance, version, iid, output, outer);
    logf("DirectInput8Create instance=%p version=%lX IID=%08lX hr=%08lX obj=%p", instance, version, iid.Data1, hr, *output);
    if (FAILED(hr) && LocalInput::supports(iid) && !outer)
    {
        configureInputBridge({ mapTetraMouse, takeMousePresses, settingsPanelOpen, inputContentRect });
        *output = new LocalInput();
        logf("Using local keyboard/mouse bridge");
        return S_OK;
    }
    return hr;
}

} // namespace tmloader
