// A keyboard and mouse reporting what the host hands kr_dinput_*, and no game pads for the client's pad setup to find.
#include <dinput.h>

#include "com.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>

extern "C"
{
    const DIDATAFORMAT c_dfDIKeyboard  = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), 0, 256, 0, nullptr };
    const DIDATAFORMAT c_dfDIMouse     = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDFT_RELAXIS, sizeof(DIMOUSESTATE), 0, nullptr };
    const DIDATAFORMAT c_dfDIJoystick2 = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDFT_ABSAXIS, sizeof(DIJOYSTATE2), 0, nullptr };
}

namespace
{

enum class Kind
{
    Keyboard,
    Mouse
};

// The types that differ between the ANSI and wide interfaces, so one template implements both.
struct DIAnsiTypes
{
    using String                         = LPCSTR;
    using DeviceInstance                 = DIDEVICEINSTANCEA;
    using ObjectInstance                 = DIDEVICEOBJECTINSTANCEA;
    using EffectInfo                     = DIEFFECTINFOA;
    using ActionFormat                   = LPDIACTIONFORMATA;
    using ImageInfoHeader                = LPDIDEVICEIMAGEINFOHEADERA;
    using ConfigureParams                = LPDICONFIGUREDEVICESPARAMSA;
    using Device                         = IDirectInputDevice8A;
    using EnumObjectsCallback            = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffectsCallback            = LPDIENUMEFFECTSCALLBACKA;
    using EnumDevicesCallback            = LPDIENUMDEVICESCALLBACKA;
    using EnumDevicesBySemanticsCallback = LPDIENUMDEVICESBYSEMANTICSCBA;
};

struct DIWideTypes
{
    using String                         = LPCWSTR;
    using DeviceInstance                 = DIDEVICEINSTANCEW;
    using ObjectInstance                 = DIDEVICEOBJECTINSTANCEW;
    using EffectInfo                     = DIEFFECTINFOW;
    using ActionFormat                   = LPDIACTIONFORMATW;
    using ImageInfoHeader                = LPDIDEVICEIMAGEINFOHEADERW;
    using ConfigureParams                = LPDICONFIGUREDEVICESPARAMSW;
    using Device                         = IDirectInputDevice8W;
    using EnumObjectsCallback            = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffectsCallback            = LPDIENUMEFFECTSCALLBACKW;
    using EnumDevicesCallback            = LPDIENUMDEVICESCALLBACKW;
    using EnumDevicesBySemanticsCallback = LPDIENUMDEVICESBYSEMANTICSCBW;
};

// The buffered events keep a press that came and went between two frames.
struct Input
{
    BYTE                           keys[256] = {};
    DIMOUSESTATE                   mouse     = {};
    std::deque<DIDEVICEOBJECTDATA> keyEvents;
    std::deque<DIDEVICEOBJECTDATA> mouseEvents;
};

auto input = Input{};

constexpr auto Pressed(bool down) -> BYTE
{
    if (down)
    {
        return 0x80;
    }

    return 0;
}

auto Events(Kind kind) -> std::deque<DIDEVICEOBJECTDATA>&
{
    if (kind == Kind::Keyboard)
    {
        return input.keyEvents;
    }

    return input.mouseEvents;
}

auto Queue(Kind kind, DWORD offset, bool down) -> void
{
    auto& events = Events(kind);
    // DIPROP_BUFFERSIZE's worth: a client that stops reading does not grow it forever
    if (events.size() >= 256)
    {
        events.pop_front();
    }

    auto event   = DIDEVICEOBJECTDATA{};
    event.dwOfs  = offset;
    event.dwData = Pressed(down);
    events.push_back(event);
}

auto CopyName(CHAR* to, const char* from) -> void
{
    std::strncpy(to, from, MAX_PATH - 1);
}

auto CopyName(WCHAR* to, const char* from) -> void
{
    size_t i = 0;
    for (; from[i] != '\0' && i < MAX_PATH - 1; ++i)
    {
        to[i] = static_cast<WCHAR>(from[i]);
    }

    to[i] = L'\0';
}

template <class Instance>
auto Describe(Kind kind, Instance& instance) -> void
{
    std::memset(&instance, 0, sizeof(instance));
    instance.dwSize = sizeof(instance);
    if (kind == Kind::Keyboard)
    {
        instance.guidInstance = instance.guidProduct = GUID_SysKeyboard;
        instance.dwDevType                           = DI8DEVTYPE_KEYBOARD;
        CopyName(instance.tszInstanceName, "Keyboard");
        CopyName(instance.tszProductName, "Keyboard");
        return;
    }

    instance.guidInstance = instance.guidProduct = GUID_SysMouse;
    instance.dwDevType                           = DI8DEVTYPE_MOUSE;
    CopyName(instance.tszInstanceName, "Mouse");
    CopyName(instance.tszProductName, "Mouse");
}

template <class Interface, class T>
class InputDevice final : public nulldx::Object<Interface>
{
public:
    InputDevice(Kind kind, REFIID iid)
    : kind_(kind)
    , iid_(iid)
    {
    }

    auto Implements(REFIID riid) const -> bool override
    {
        return riid == iid_;
    }

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override
    {
        if (caps == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        const auto size = caps->dwSize;
        std::memset(caps, 0, sizeof(*caps));
        caps->dwSize  = size;
        caps->dwFlags = DIDC_ATTACHED;
        if (kind_ == Kind::Keyboard)
        {
            caps->dwDevType = DI8DEVTYPE_KEYBOARD;
            caps->dwAxes    = 0;
            caps->dwButtons = 128;
        }
        else
        {
            caps->dwDevType = DI8DEVTYPE_MOUSE;
            caps->dwAxes    = 3;
            caps->dwButtons = 8;
        }

        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(
        typename T::EnumObjectsCallback callback, LPVOID context, DWORD flags) override
    {
        return DI_OK;
    }

    // Only the pad settings read a property, and there are no pads; what the client sets needs no keeping.
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, LPDIPROPHEADER) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID, LPCDIPROPHEADER) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Acquire() override
    {
        if (std::exchange(acquired_, true))
        {
            return S_FALSE;
        }

        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Unacquire() override
    {
        if (std::exchange(acquired_, false))
        {
            return DI_OK;
        }

        return DI_NOEFFECT;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID state) override
    {
        if (state == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        if (!acquired_)
        {
            return DIERR_NOTACQUIRED;
        }

        std::memset(state, 0, size);
        if (kind_ == Kind::Keyboard)
        {
            std::memcpy(state, input.keys, std::min<size_t>(size, sizeof(input.keys)));
        }
        else
        {
            std::memcpy(state, &input.mouse, std::min<size_t>(size, sizeof(input.mouse)));
            // Relative axes report the motion since the last read
            input.mouse.lX = 0;
            input.mouse.lY = 0;
            input.mouse.lZ = 0;
        }

        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) override
    {
        if (count == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        if (!acquired_)
        {
            return DIERR_NOTACQUIRED;
        }

        auto&      events = Events(kind_);
        const auto n      = std::min<DWORD>(*count, static_cast<DWORD>(events.size()));
        for (DWORD i = 0; i < n; ++i)
        {
            if (data != nullptr)
            {
                std::memcpy(reinterpret_cast<BYTE*>(data) + i * size, &events[i], std::min<size_t>(size, sizeof(DIDEVICEOBJECTDATA)));
            }
        }

        if ((flags & DIGDD_PEEK) == 0)
        {
            events.erase(events.begin(), events.begin() + n);
        }

        *count = n;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override
    {
        if (format == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        if (acquired_)
        {
            return DIERR_ACQUIRED;
        }

        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::ObjectInstance* instance, DWORD object, DWORD how) override
    {
        return DIERR_OBJECTNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DeviceInstance* instance) override
    {
        if (instance == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        Describe(kind_, *instance);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override
    {
        return DI_OK;
    }

    // Keyboards and mice have no force feedback.
    HRESULT STDMETHODCALLTYPE CreateEffect(
        REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override
    {
        if (out != nullptr)
        {
            *out = nullptr;
        }

        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffectsCallback callback, LPVOID context, DWORD type) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffectInfo* info, REFGUID guid) override
    {
        return DIERR_DEVICENOTREG;
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(
        LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID context, DWORD flags) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Poll() override
    {
        if (!acquired_)
        {
            return DIERR_NOTACQUIRED;
        }

        return DI_NOEFFECT;
    }

    HRESULT STDMETHODCALLTYPE SendDeviceData(
        DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(
        typename T::String file, LPDIENUMEFFECTSINFILECALLBACK callback, LPVOID context, DWORD flags) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile(
        typename T::String file, DWORD count, LPDIFILEEFFECT effects, DWORD flags) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE BuildActionMap(
        typename T::ActionFormat format, typename T::String user, DWORD flags) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetActionMap(
        typename T::ActionFormat format, typename T::String user, DWORD flags) override
    {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImageInfoHeader header) override
    {
        return DIERR_UNSUPPORTED;
    }

private:
    Kind       kind_;
    const IID& iid_;
    bool       acquired_ = false;
};

template <class Interface, class T>
class DirectInput final : public nulldx::Object<Interface>
{
public:
    DirectInput(REFIID iid, REFIID deviceIid)
    : iid_(iid)
    , deviceIid_(deviceIid)
    {
    }

    auto Implements(REFIID riid) const -> bool override
    {
        return riid == iid_;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, typename T::Device** device, LPUNKNOWN outer) override
    {
        if (device == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        *device = nullptr;
        if (guid != GUID_SysKeyboard && guid != GUID_SysMouse)
        {
            return DIERR_DEVICENOTREG;
        }

        const auto kind = [&]() -> Kind
        {
            if (guid == GUID_SysKeyboard)
            {
                return Kind::Keyboard;
            }

            return Kind::Mouse;
        }();

        *device = new InputDevice<typename T::Device, T>(kind, deviceIid_);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(
        DWORD type, typename T::EnumDevicesCallback callback, LPVOID context, DWORD flags) override
    {
        if (callback == nullptr)
        {
            return DIERR_INVALIDPARAM;
        }

        if ((flags & DIEDFL_FORCEFEEDBACK) != 0)
        {
            return DI_OK;
        }

        for (const auto kind : { Kind::Keyboard, Kind::Mouse })
        {
            const auto deviceClass = [&]() -> DWORD
            {
                if (kind == Kind::Keyboard)
                {
                    return DI8DEVCLASS_KEYBOARD;
                }

                return DI8DEVCLASS_POINTER;
            }();

            if (type != DI8DEVCLASS_ALL && type != deviceClass)
            {
                continue;
            }

            typename T::DeviceInstance instance;
            Describe(kind, instance);
            if (callback(&instance, context) == DIENUM_STOP)
            {
                break;
            }
        }

        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override
    {
        if (guid != GUID_SysKeyboard && guid != GUID_SysMouse)
        {
            return DIERR_DEVICENOTREG;
        }

        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guid, typename T::String name, LPGUID instance) override
    {
        return DIERR_DEVICENOTREG;
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::String user, typename T::ActionFormat format, typename T::EnumDevicesBySemanticsCallback callback, LPVOID context, DWORD flags) override
    {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(
        LPDICONFIGUREDEVICESCALLBACK callback, typename T::ConfigureParams params, DWORD flags, LPVOID data) override
    {
        return DIERR_UNSUPPORTED;
    }

private:
    const IID& iid_;
    const IID& deviceIid_;
};

} // namespace

extern "C" auto kr_dinput_key(int key, bool down) -> void
{
    if (key <= 0 || key > 0xFF || ((input.keys[key] & 0x80) != 0) == down)
    {
        return;
    }

    input.keys[key] = Pressed(down);
    Queue(Kind::Keyboard, static_cast<DWORD>(key), down);
}

extern "C" auto kr_dinput_mouse(int dx, int dy, int dz) -> void
{
    input.mouse.lX += dx;
    input.mouse.lY += dy;
    input.mouse.lZ += dz;
}

extern "C" auto kr_dinput_button(int button, bool down) -> void
{
    if (button < 0 || button > 3 || ((input.mouse.rgbButtons[button] & 0x80) != 0) == down)
    {
        return;
    }

    input.mouse.rgbButtons[button] = Pressed(down);
    Queue(Kind::Mouse, DIMOFS_BUTTON0 + button, down);
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    if (ppvOut == nullptr)
    {
        return DIERR_INVALIDPARAM;
    }

    *ppvOut = nullptr;
    if (punkOuter != nullptr)
    {
        return DIERR_NOAGGREGATION;
    }

    if (dwVersion < 0x0800)
    {
        return DIERR_OLDDIRECTINPUTVERSION;
    }

    if (riidltf == IID_IDirectInput8A)
    {
        *ppvOut = new DirectInput<IDirectInput8A, DIAnsiTypes>(IID_IDirectInput8A, IID_IDirectInputDevice8A);
    }
    else if (riidltf == IID_IDirectInput8W)
    {
        *ppvOut = new DirectInput<IDirectInput8W, DIWideTypes>(IID_IDirectInput8W, IID_IDirectInputDevice8W);
    }
    else
    {
        return DIERR_NOINTERFACE;
    }

    return DI_OK;
}
