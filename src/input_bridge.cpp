#include "input_bridge.h"
#include "runtime.h"

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

HRESULT STDMETHODCALLTYPE LocalInput::QueryInterface(REFIID iid, void** out)
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

ULONG STDMETHODCALLTYPE LocalInput::AddRef()
{
    return ++references_;
}

ULONG STDMETHODCALLTYPE LocalInput::Release()
{
    const auto n = --references_;
    if (!n)
    {
        delete this;
    }
    return n;
}

HRESULT STDMETHODCALLTYPE LocalInput::CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer)
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

HRESULT STDMETHODCALLTYPE LocalInput::EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInput::GetDeviceStatus(REFGUID guid)
{
    if (guid == keyboardGUID || guid == mouseGUID)
    {
        return S_OK;
    }
    return DI_NOTATTACHED;
}

HRESULT STDMETHODCALLTYPE LocalInput::RunControlPanel(HWND, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInput::Initialize(HINSTANCE, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInput::FindDevice(REFGUID, LPCSTR, LPGUID)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInput::EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInput::ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID)
{
    return E_NOTIMPL;
}

LocalInputDevice::LocalInputDevice(bool key) noexcept
: keyboard_(key)
{
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::QueryInterface(REFIID, void** out)
{
    *out = this;
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE LocalInputDevice::AddRef()
{
    return ++references_;
}

ULONG STDMETHODCALLTYPE LocalInputDevice::Release()
{
    const auto n = --references_;
    if (!n)
    {
        delete this;
    }
    return n;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetCapabilities(LPDIDEVCAPS p)
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

HRESULT STDMETHODCALLTYPE LocalInputDevice::EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetProperty(REFGUID, LPDIPROPHEADER)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SetProperty(REFGUID, LPCDIPROPHEADER)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::Acquire()
{
    mouseSampleValid_ = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::Unacquire()
{
    mouseSampleValid_ = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetDeviceState(DWORD size, void* output)
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

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD)
{
    if (count)
    {
        *count = 0;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SetDataFormat(LPCDIDATAFORMAT p)
{
    logf("Input bridge format bytes=%lu objects=%lu flags=%lX", p->dwDataSize, p->dwNumObjs, p->dwFlags);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SetEventNotification(HANDLE)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SetCooperativeLevel(HWND h, DWORD flags)
{
    window_           = h;
    mouseSampleValid_ = false;
    logf("Input bridge cooperative window=%p flags=%lX", h, flags);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetDeviceInfo(LPDIDEVICEINSTANCEA p)
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

HRESULT STDMETHODCALLTYPE LocalInputDevice::RunControlPanel(HWND, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::Initialize(HINSTANCE, DWORD, REFGUID)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetEffectInfo(LPDIEFFECTINFOA, REFGUID)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetForceFeedbackState(LPDWORD p)
{
    if (p)
    {
        *p = 0;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SendForceFeedbackCommand(DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::Escape(LPDIEFFESCAPE)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::Poll()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD count, DWORD)
{
    if (count)
    {
        *count = 0;
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LocalInputDevice::GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA)
{
    return E_NOTIMPL;
}

} // namespace tmloader
