#include "app/hooks.h"
#include "app/log.h"
#include "input/input_bridge.h"
#include "platform/patch.h"

namespace tmloader
{
namespace
{

constexpr DWORD directInputCreateImport = 0x204014;

// Native input object; +0x2C/+0x2E hold the software cursor position and
// +0x14/+0x18 select keyboard-driven cursor animation over relative movement.
constexpr DWORD inputState     = 0x2ff570;
constexpr DWORD cursorAnimate  = 0x14;
constexpr DWORD cursorAnimateY = 0x18;
constexpr DWORD cursorX        = 0x2c;
constexpr DWORD cursorY        = 0x2e;

// The game only reads the mouse through DirectInput; keyboard input arrives as window messages.
auto WINAPI hookedDirectInput8Create(HINSTANCE, DWORD version, REFIID iid, void** output, IUnknown* outer) -> HRESULT
{
    logLine("DirectInput8Create version={:X} IID={:08X}", version, iid.Data1);
    if (outer || !LocalInput::supports(iid))
    {
        return DIERR_NOINTERFACE;
    }

    *output = new LocalInput();
    return S_OK;
}

} // namespace

auto mapGameMouse(LONG x, LONG y, LONG* dx, LONG* dy) -> bool
{
    if (!gameBase || !gameWindow)
    {
        return false;
    }

    // Real pointer input cancels keyboard-driven cursor animation.
    auto* const input                                = gameBase + inputState;
    *reinterpret_cast<LONG*>(input + cursorAnimate)  = 0;
    *reinterpret_cast<LONG*>(input + cursorAnimateY) = 0;
    *dx                                              = x - *reinterpret_cast<const SHORT*>(input + cursorX);
    *dy                                              = y - *reinterpret_cast<const SHORT*>(input + cursorY);
    return true;
}

void installInputHooks(HMODULE game)
{
    setImport(game, directInputCreateImport, reinterpret_cast<void*>(hookedDirectInput8Create));
}

} // namespace tmloader
