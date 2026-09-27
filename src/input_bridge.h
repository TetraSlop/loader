#pragma once
// Internal keyboard/mouse fallback; native DirectInput is preferred.
// No hooks or background input capture: only reads input while our own window has focus.
using LocalMouseMapper = bool (*)(LONG x, LONG y, LONG* dx, LONG* dy);
static LocalMouseMapper localMouseMapper = nullptr;
static BYTE (*localMousePresses)() = nullptr;
static bool (*localInputBlocked)() = nullptr;
static bool (*localContentRect)(RECT*) = nullptr;
class LocalInputDevice : public IDirectInputDevice8A
{
    ULONG refs = 1;
    bool keyboard;
    HWND window = nullptr;
    POINT previous = {};
    bool mouseSampleValid = false;
    LONG previousWidth = 0, previousHeight = 0;
    BYTE previousButtons = 0;

  public:
    explicit LocalInputDevice(bool key) : keyboard(key)
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override
    {
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS p) override
    {
        const auto size = p->dwSize;
        memset(p, 0, size);
        p->dwSize = size;
        p->dwFlags = DIDC_ATTACHED;
        p->dwDevType = keyboard ? DI8DEVTYPE_KEYBOARD : DI8DEVTYPE_MOUSE;
        p->dwAxes = keyboard ? 0 : 2;
        p->dwButtons = keyboard ? 256 : 3;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, LPDIPROPHEADER) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID, LPCDIPROPHEADER) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Acquire() override
    {
        mouseSampleValid = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override
    {
        mouseSampleValid = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, void *output) override
    {
        if (!output)
            return E_POINTER;
        memset(output, 0, size);
        const auto foreground = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        if (pid != GetCurrentProcessId() || (window && foreground != window) ||
            (localInputBlocked && localInputBlocked()))
        {
            mouseSampleValid = false;
            if (localMousePresses) localMousePresses();
            return S_OK;
        }
        if (keyboard && size == 256)
        {
            auto* const keys = static_cast<BYTE*>(output);
            for (UINT scan = 1; scan < 256; scan++)
            {
                const UINT code = (scan & 0x7f) | ((scan & 0x80) ? 0xe000 : 0);
                const auto vk = MapVirtualKeyA(code, MAPVK_VSC_TO_VK_EX);
                if (vk == VK_F10 || vk == VK_F12 || (vk == VK_RETURN && (GetAsyncKeyState(VK_MENU) & 0x8000))) continue;
                if (vk && (GetAsyncKeyState(vk) & 0x8000))
                    keys[scan] = 0x80;
            }
        }
        else if (!keyboard && size >= sizeof(DIMOUSESTATE))
        {
            POINT current = {};
            RECT client = {};
            // Sample in the same client coordinate space as the rendered game.
            // Never turn desktop movement during a focus/window transition into
            // a large relative movement on the first active frame.
            if (!window || !GetCursorPos(&current) || !ScreenToClient(window, &current) ||
                !GetClientRect(window, &client) || (localContentRect && !localContentRect(&client)) ||
                !PtInRect(&client, current))
            {
                mouseSampleValid = false;
                if (localMousePresses) localMousePresses();
                return S_OK;
            }
            const auto width = client.right - client.left;
            const auto height = client.bottom - client.top;
            if (width <= 0 || height <= 0)
            {
                mouseSampleValid = false;
                return S_OK;
            }
            auto* const m = static_cast<DIMOUSESTATE*>(output);
            current.x -= client.left;
            current.y -= client.top;
            const BYTE pressed = localMousePresses ? localMousePresses() : 0;
            m->rgbButtons[0] = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (pressed & 1)) ? 0x80 : 0;
            m->rgbButtons[1] = ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) || (pressed & 2)) ? 0x80 : 0;
            const BYTE buttons = (m->rgbButtons[0] ? 1 : 0) | (m->rgbButtons[1] ? 2 : 0);
            const bool pointerMoved = !mouseSampleValid || current.x != previous.x || current.y != previous.y ||
                                width != previousWidth || height != previousHeight;
            const bool pointerClicked = !mouseSampleValid ? buttons != 0 : (buttons & ~previousButtons) != 0;
            if (mouseSampleValid && width == previousWidth && height == previousHeight)
            {
                m->lX = MulDiv(current.x, 640, width) - MulDiv(previous.x, 640, width);
                m->lY = MulDiv(current.y, 480, height) - MulDiv(previous.y, 480, height);
            }
            // UI keyboard navigation can move the software cursor independently.
            // A real pointer move/click takes control again using the client's
            // existing relative-input integration, not an OS pointer warp.
            if (localMouseMapper && (pointerMoved || pointerClicked))
                localMouseMapper(min(639, MulDiv(current.x, 640, width)),
                                 min(479, MulDiv(current.y, 480, height)), &m->lX, &m->lY);
            previous = current;
            previousWidth = width;
            previousHeight = height;
            mouseSampleValid = true;
            previousButtons = buttons;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) override
    {
        if (count)
            *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT p) override
    {
        logf("Input bridge format bytes=%lu objects=%lu flags=%lX", p->dwDataSize, p->dwNumObjs, p->dwFlags);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD flags) override
    {
        window = h;
        mouseSampleValid = false;
        logf("Input bridge cooperative window=%p flags=%lX", h, flags);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA p) override
    {
        const auto size = p->dwSize;
        memset(p, 0, size);
        p->dwSize = size;
        p->guidInstance = keyboard ? keyboardGUID : mouseGUID;
        p->guidProduct = p->guidInstance;
        p->dwDevType = keyboard ? DI8DEVTYPE_KEYBOARD : DI8DEVTYPE_MOUSE;
        strcpy_s(p->tszInstanceName, keyboard ? "Local keyboard" : "Local mouse");
        strcpy_s(p->tszProductName, p->tszInstanceName);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT *, LPUNKNOWN) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD p) override
    {
        if (p)
            *p = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Poll() override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD count, DWORD) override
    {
        if (count)
            *count = 0;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override
    {
        return E_NOTIMPL;
    }
};
class LocalInput : public IDirectInput8A
{
    ULONG refs = 1;

  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
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
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A *out, LPUNKNOWN outer) override
    {
        if (outer)
            return CLASS_E_NOAGGREGATION;
        if (guid != keyboardGUID && guid != mouseGUID)
        {
            *out = nullptr;
            return DIERR_DEVICENOTREG;
        }
        *out = new LocalInputDevice(guid == keyboardGUID);
        logf("Input bridge created %s", guid == keyboardGUID ? "keyboard" : "mouse");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override
    {
        return guid == keyboardGUID || guid == mouseGUID ? S_OK : DI_NOTATTACHED;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID, LPCSTR, LPGUID) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID,
                                                     DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD,
                                               LPVOID) override
    {
        return E_NOTIMPL;
    }
};
