#pragma once
#include <windows.h>

#include <dinput.h>

namespace tmloader
{

using LocalMouseMapper = bool (*)(LONG x, LONG y, LONG* dx, LONG* dy);

struct InputBridgeCallbacks
{
    LocalMouseMapper mouseMapper = nullptr;
    BYTE (*mousePresses)()       = nullptr;
    bool (*inputBlocked)()       = nullptr;
    bool (*contentRect)(RECT*)   = nullptr;
};

void configureInputBridge(const InputBridgeCallbacks& callbacks) noexcept;

// Focus-aware fallback for the immediate keyboard/mouse path used by the client.
class LocalInputDevice final : public IDirectInputDevice8A
{
public:
    ~LocalInputDevice()                                          = default;
    LocalInputDevice(const LocalInputDevice&)                    = delete;
    auto operator=(const LocalInputDevice&) -> LocalInputDevice& = delete;
    LocalInputDevice(LocalInputDevice&&)                         = delete;
    auto operator=(LocalInputDevice&&) -> LocalInputDevice&      = delete;
    explicit LocalInputDevice(bool key) noexcept;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE   AddRef() override;
    ULONG STDMETHODCALLTYPE   Release() override;
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS p) override;
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, LPDIPROPHEADER) override;
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID, LPCDIPROPHEADER) override;
    HRESULT STDMETHODCALLTYPE Acquire() override;
    HRESULT STDMETHODCALLTYPE Unacquire() override;
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, void* output) override;
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT p) override;
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override;
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA p) override;
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override;
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override;
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override;
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override;
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD p) override;
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override;
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override;
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override;
    HRESULT STDMETHODCALLTYPE Poll() override;
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD count, DWORD) override;
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override;
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override;
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override;

private:
    ULONG      references_ = 1;
    const bool keyboard_;
    HWND       window_           = nullptr;
    POINT      previous_         = {};
    bool       mouseSampleValid_ = false;
    LONG       previousWidth_    = 0;
    LONG       previousHeight_   = 0;
    BYTE       previousButtons_  = 0;
};

class LocalInput final : public IDirectInput8A
{
public:
    LocalInput()                                                     = default;
    ~LocalInput()                                                    = default;
    LocalInput(const LocalInput&)                                    = delete;
    auto operator=(const LocalInput&) -> LocalInput&                 = delete;
    LocalInput(LocalInput&&)                                         = delete;
    auto                      operator=(LocalInput&&) -> LocalInput& = delete;
    static auto               supports(REFIID iid) noexcept -> bool;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
    ULONG STDMETHODCALLTYPE   AddRef() override;
    ULONG STDMETHODCALLTYPE   Release() override;
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override;
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override;
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) override;
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID, LPCSTR, LPGUID) override;
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD) override;
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID) override;

private:
    ULONG references_ = 1;
};

} // namespace tmloader
