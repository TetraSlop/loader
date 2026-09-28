#include "input/input_bridge.h"
#include "app/runtime.h"

namespace tmloader
{
namespace
{

constexpr auto       inputIID     = IID{ 0xbf798030, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
constexpr auto       keyboardGUID = GUID{ 0x6f1d2b61, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
constexpr auto       mouseGUID    = GUID{ 0x6f1d2b60, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
InputBridgeCallbacks callbacks;

} // namespace

void configureInputBridge(const InputBridgeCallbacks& value) noexcept
{
    callbacks = value;
}

auto LocalInput::supports(REFIID iid) noexcept -> bool
{
    return iid == inputIID;
}

auto STDMETHODCALLTYPE LocalInput::QueryInterface(REFIID iid, void** out) -> HRESULT
{
    if (iid != inputIID && iid != IID_IUnknown)
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    *out = this;
    AddRef();
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::AddRef() -> ULONG
{
    return ++references_;
}

auto STDMETHODCALLTYPE LocalInput::Release() -> ULONG
{
    const auto n = --references_;
    if (!n)
    {
        delete this;
    }
    return n;
}

auto STDMETHODCALLTYPE LocalInput::CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) -> HRESULT
{
    if (outer)
    {
        return CLASS_E_NOAGGREGATION;
    }
    if (guid != keyboardGUID && guid != mouseGUID)
    {
        *out = nullptr;
        return DIERR_DEVICENOTREG;
    }
    *out = new LocalInputDevice(guid == keyboardGUID);
    if (guid == keyboardGUID)
    {
        logf("Input bridge created keyboard");
    }
    else
    {
        logf("Input bridge created mouse");
    }
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInput::GetDeviceStatus(REFGUID guid) -> HRESULT
{
    if (guid == keyboardGUID || guid == mouseGUID)
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

LocalInputDevice::LocalInputDevice(bool key) noexcept
: keyboard_(key)
{
}

auto STDMETHODCALLTYPE LocalInputDevice::QueryInterface(REFIID, void** out) -> HRESULT
{
    *out = this;
    AddRef();
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::AddRef() -> ULONG
{
    return ++references_;
}

auto STDMETHODCALLTYPE LocalInputDevice::Release() -> ULONG
{
    const auto n = --references_;
    if (!n)
    {
        delete this;
    }
    return n;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetCapabilities(LPDIDEVCAPS p) -> HRESULT
{
    const auto size = p->dwSize;
    memset(p, 0, size);
    p->dwSize  = size;
    p->dwFlags = DIDC_ATTACHED;
    if (keyboard_)
    {
        p->dwDevType = DI8DEVTYPE_KEYBOARD;
        p->dwAxes    = 0;
        p->dwButtons = 256;
    }
    else
    {
        p->dwDevType = DI8DEVTYPE_MOUSE;
        p->dwAxes    = 2;
        p->dwButtons = 3;
    }
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetProperty(REFGUID, LPDIPROPHEADER) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::SetProperty(REFGUID, LPCDIPROPHEADER) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::Acquire() -> HRESULT
{
    mouseSampleValid_ = false;
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::Unacquire() -> HRESULT
{
    mouseSampleValid_ = false;
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetDeviceState(DWORD size, void* output) -> HRESULT
{
    if (!output)
    {
        return E_POINTER;
    }
    memset(output, 0, size);
    const auto foreground = GetForegroundWindow();
    auto       pid        = DWORD{ 0 };
    GetWindowThreadProcessId(foreground, &pid);
    if (pid != GetCurrentProcessId() || (window_ && foreground != window_) ||
        (callbacks.inputBlocked && callbacks.inputBlocked()))
    {
        mouseSampleValid_ = false;
        if (callbacks.mousePresses)
        {
            callbacks.mousePresses();
        }
        return S_OK;
    }
    if (keyboard_ && size == 256)
    {
        auto* const keys = static_cast<BYTE*>(output);
        for (auto scan = UINT{ 1 }; scan < 256; scan++)
        {
            const auto code = [scan]() -> UINT
            {
                if (scan & 0x80)
                {
                    return (scan & 0x7f) | 0xe000;
                }
                return scan & 0x7f;
            }();
            const auto vk = MapVirtualKeyA(code, MAPVK_VSC_TO_VK_EX);
            if (vk == VK_F10 || vk == VK_F12 || (vk == VK_RETURN && (GetAsyncKeyState(VK_MENU) & 0x8000)))
            {
                continue;
            }
            if (vk && (GetAsyncKeyState(vk) & 0x8000))
            {
                keys[scan] = 0x80;
            }
        }
    }
    else if (!keyboard_ && size >= sizeof(DIMOUSESTATE))
    {
        auto current = POINT{};
        auto client  = RECT{};
        // Sample in the same client coordinate space as the rendered game.
        // Never turn desktop movement during a focus/window transition into
        // a large relative movement on the first active frame.
        if (!window_ || !GetCursorPos(&current) || !ScreenToClient(window_, &current) ||
            !GetClientRect(window_, &client) || (callbacks.contentRect && !callbacks.contentRect(&client)) ||
            !PtInRect(&client, current))
        {
            mouseSampleValid_ = false;
            if (callbacks.mousePresses)
            {
                callbacks.mousePresses();
            }
            return S_OK;
        }
        const auto width  = client.right - client.left;
        const auto height = client.bottom - client.top;
        if (width <= 0 || height <= 0)
        {
            mouseSampleValid_ = false;
            return S_OK;
        }
        auto* const m = static_cast<DIMOUSESTATE*>(output);
        current.x -= client.left;
        current.y -= client.top;
        const auto pressed = []() -> BYTE
        {
            if (callbacks.mousePresses)
            {
                return callbacks.mousePresses();
            }
            return 0;
        }();
        auto buttons = BYTE{ 0 };
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (pressed & 1))
        {
            m->rgbButtons[0] = 0x80;
            buttons |= 1;
        }
        if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) || (pressed & 2))
        {
            m->rgbButtons[1] = 0x80;
            buttons |= 2;
        }
        const auto pointerMoved   = !mouseSampleValid_ || current.x != previous_.x || current.y != previous_.y ||
                                    width != previousWidth_ || height != previousHeight_;
        const auto pointerClicked = [&]() -> bool
        {
            if (!mouseSampleValid_)
            {
                return buttons != 0;
            }
            return (buttons & ~previousButtons_) != 0;
        }();
        if (mouseSampleValid_ && width == previousWidth_ && height == previousHeight_)
        {
            m->lX = MulDiv(current.x, 640, width) - MulDiv(previous_.x, 640, width);
            m->lY = MulDiv(current.y, 480, height) - MulDiv(previous_.y, 480, height);
        }
        // UI keyboard navigation can move the software cursor independently.
        // A real pointer move/click takes control again using the client's
        // existing relative-input integration, not an OS pointer warp.
        if (callbacks.mouseMapper && (pointerMoved || pointerClicked))
        {
            callbacks.mouseMapper(min(639, MulDiv(current.x, 640, width)),
                                  min(479, MulDiv(current.y, 480, height)),
                                  &m->lX,
                                  &m->lY);
        }
        previous_         = current;
        previousWidth_    = width;
        previousHeight_   = height;
        mouseSampleValid_ = true;
        previousButtons_  = buttons;
    }
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) -> HRESULT
{
    if (count)
    {
        *count = 0;
    }
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::SetDataFormat(LPCDIDATAFORMAT p) -> HRESULT
{
    logf("Input bridge format bytes=%lu objects=%lu flags=%lX", p->dwDataSize, p->dwNumObjs, p->dwFlags);
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::SetEventNotification(HANDLE) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::SetCooperativeLevel(HWND h, DWORD flags) -> HRESULT
{
    window_           = h;
    mouseSampleValid_ = false;
    logf("Input bridge cooperative window=%p flags=%lX", h, flags);
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetDeviceInfo(LPDIDEVICEINSTANCEA p) -> HRESULT
{
    const auto size = p->dwSize;
    memset(p, 0, size);
    p->dwSize = size;
    if (keyboard_)
    {
        p->guidInstance = keyboardGUID;
        p->dwDevType    = DI8DEVTYPE_KEYBOARD;
        strcpy_s(p->tszInstanceName, "Local keyboard");
    }
    else
    {
        p->guidInstance = mouseGUID;
        p->dwDevType    = DI8DEVTYPE_MOUSE;
        strcpy_s(p->tszInstanceName, "Local mouse");
    }
    p->guidProduct = p->guidInstance;
    strcpy_s(p->tszProductName, p->tszInstanceName);
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::RunControlPanel(HWND, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::Initialize(HINSTANCE, DWORD, REFGUID) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetEffectInfo(LPDIEFFECTINFOA, REFGUID) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetForceFeedbackState(LPDWORD p) -> HRESULT
{
    if (p)
    {
        *p = 0;
    }
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::SendForceFeedbackCommand(DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::Escape(LPDIEFFESCAPE) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::Poll() -> HRESULT
{
    return S_OK;
}

auto STDMETHODCALLTYPE LocalInputDevice::SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD count, DWORD) -> HRESULT
{
    if (count)
    {
        *count = 0;
    }
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) -> HRESULT
{
    return E_NOTIMPL;
}

auto STDMETHODCALLTYPE LocalInputDevice::GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) -> HRESULT
{
    return E_NOTIMPL;
}

} // namespace tmloader
