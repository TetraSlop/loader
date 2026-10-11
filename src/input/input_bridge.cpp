#include "input/input_bridge.h"
#include "app/log.h"
#include "ui/window.h"
#include <algorithm>
#include <cstring>

namespace tmloader
{

auto LocalInput::supports(REFIID iid) noexcept -> bool
{
    return iid == IID_IDirectInput8A;
}

auto STDMETHODCALLTYPE LocalInput::CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) -> HRESULT
{
    if (outer)
    {
        return CLASS_E_NOAGGREGATION;
    }

    if (guid != GUID_SysMouse)
    {
        *out = nullptr;
        return DIERR_DEVICENOTREG;
    }

    Microsoft::WRL::ComPtr<IDirectInputDevice8A> mouse = Microsoft::WRL::Make<LocalMouse>();
    *out                                               = mouse.Detach();
    if (!*out)
    {
        return E_OUTOFMEMORY;
    }

    logLine("Input bridge created mouse");
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::GetDeviceStatus(REFGUID guid) -> HRESULT
{
    if (guid == GUID_SysMouse)
    {
        return S_OK;
    }

    return DI_NOTATTACHED;
}

auto STDMETHODCALLTYPE LocalInput::RunControlPanel(HWND, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInput::Initialize(HINSTANCE, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::FindDevice(REFGUID, LPCSTR, LPGUID) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInput::EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::QueryInterface(REFIID, void** out) -> HRESULT
{
    *out = static_cast<IDirectInputDevice8A*>(this);
    AddRef();
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::GetCapabilities(LPDIDEVCAPS p) -> HRESULT
{
    const auto size = p->dwSize;
    std::memset(p, 0, size);
    p->dwSize    = size;
    p->dwFlags   = DIDC_ATTACHED;
    p->dwDevType = DI8DEVTYPE_MOUSE;
    p->dwAxes    = 2;
    p->dwButtons = 3;

    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::GetProperty(REFGUID, LPDIPROPHEADER) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::SetProperty(REFGUID, LPCDIPROPHEADER) -> HRESULT
{
    return S_OK;
}

void LocalMouse::invalidateSample() noexcept
{
    mouseSampleValid_ = false;
}

auto STDMETHODCALLTYPE LocalMouse::Acquire() -> HRESULT
{
    invalidateSample();
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::Unacquire() -> HRESULT
{
    invalidateSample();
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::GetDeviceState(DWORD size, void* output) -> HRESULT
{
    if (!output)
    {
        return E_POINTER;
    }

    std::memset(output, 0, size);
    const auto foreground = GetForegroundWindow();
    auto       pid        = DWORD{ 0 };
    GetWindowThreadProcessId(foreground, &pid);
    if (pid != GetCurrentProcessId() || (window_ && foreground != window_) ||
        settingsPanelOpen())
    {
        invalidateSample();
        takeMousePresses();
        return S_OK;
    }

    if (size < sizeof(DIMOUSESTATE))
    {
        return S_OK;
    }

    auto       current = POINT{};
    const auto client  = gameContentRect();
    // Outside the content area the sample is invalidated, so re-entering never jumps.
    if (!window_ || !GetCursorPos(&current) || !ScreenToClient(window_, &current) || !PtInRect(&client, current))
    {
        invalidateSample();
        takeMousePresses();
        return S_OK;
    }

    const auto width  = client.right - client.left;
    const auto height = client.bottom - client.top;

    auto* const m = static_cast<DIMOUSESTATE*>(output);
    current.x -= client.left;
    current.y -= client.top;
    const auto pressed = takeMousePresses();
    auto       buttons = BYTE{ 0 };
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (pressed & LeftButton))
    {
        m->rgbButtons[0] = 0x80;
        buttons |= LeftButton;
    }

    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) || (pressed & RightButton))
    {
        m->rgbButtons[1] = 0x80;
        buttons |= RightButton;
    }

    const auto pointerMoved   = !mouseSampleValid_ || current.x != previous_.x || current.y != previous_.y ||
                                width != previousWidth_ || height != previousHeight_;
    auto       pointerClicked = buttons != 0;
    if (mouseSampleValid_)
    {
        pointerClicked = (buttons & ~previousButtons_) != 0;
    }

    if (mouseSampleValid_ && width == previousWidth_ && height == previousHeight_)
    {
        m->lX = MulDiv(current.x, 640, width) - MulDiv(previous_.x, 640, width);
        m->lY = MulDiv(current.y, 480, height) - MulDiv(previous_.y, 480, height);
    }

    // Keyboard navigation moves the game cursor on its own; real mouse input takes it back.
    if (pointerMoved || pointerClicked)
    {
        mapGameMouse(std::min(639, MulDiv(current.x, 640, width)),
                     std::min(479, MulDiv(current.y, 480, height)),
                     &m->lX,
                     &m->lY);
    }

    previous_         = current;
    previousWidth_    = width;
    previousHeight_   = height;
    mouseSampleValid_ = true;
    previousButtons_  = buttons;

    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) -> HRESULT
{
    if (count)
    {
        *count = 0;
    }

    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::SetDataFormat(LPCDIDATAFORMAT p) -> HRESULT
{
    logLine("Input bridge format bytes={} objects={} flags={:X}", p->dwDataSize, p->dwNumObjs, p->dwFlags);
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::SetEventNotification(HANDLE) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::SetCooperativeLevel(HWND h, DWORD flags) -> HRESULT
{
    window_ = h;
    invalidateSample();
    logLine("Input bridge cooperative window={} flags={:X}", address(h), flags);
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::GetDeviceInfo(LPDIDEVICEINSTANCEA p) -> HRESULT
{
    const auto size = p->dwSize;
    std::memset(p, 0, size);
    p->dwSize       = size;
    p->guidInstance = GUID_SysMouse;
    p->guidProduct  = GUID_SysMouse;
    p->dwDevType    = DI8DEVTYPE_MOUSE;
    strcpy_s(p->tszInstanceName, "Local mouse");
    strcpy_s(p->tszProductName, "Local mouse");
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::RunControlPanel(HWND, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::Initialize(HINSTANCE, DWORD, REFGUID) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::GetEffectInfo(LPDIEFFECTINFOA, REFGUID) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::GetForceFeedbackState(LPDWORD p) -> HRESULT
{
    if (p)
    {
        *p = 0;
    }

    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::SendForceFeedbackCommand(DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::Escape(LPDIEFFESCAPE) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::Poll() -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalMouse::SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD count, DWORD) -> HRESULT
{
    if (count)
    {
        *count = 0;
    }

    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalMouse::GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) -> HRESULT
{
    return E_NOTIMPL;
}

} // namespace tmloader
