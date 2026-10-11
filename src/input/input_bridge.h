#pragma once
#include <windows.h>

#include <dinput.h>

namespace tmloader
{

// Moves the game's cursor to an absolute 640x480 position through its relative
// motion (input/input.cpp); returns false before the game is running.
auto mapGameMouse(LONG x, LONG y, LONG* dx, LONG* dy) -> bool;

// DirectInput mouse backed by the Windows pointer, mapped into the 640x480 game area.
class LocalMouse final : public IDirectInputDevice8A
{
public:
    ~LocalMouse()                                    = default;
    LocalMouse(const LocalMouse&)                    = delete;
    auto operator=(const LocalMouse&) -> LocalMouse& = delete;
    LocalMouse(LocalMouse&&)                         = delete;
    auto operator=(LocalMouse&&) -> LocalMouse&      = delete;
    LocalMouse()                                     = default;
    auto STDMETHODCALLTYPE QueryInterface(REFIID, void** out) -> HRESULT override;
    auto STDMETHODCALLTYPE AddRef() -> ULONG override;
    auto STDMETHODCALLTYPE Release() -> ULONG override;
    auto STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS p) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE GetProperty(REFGUID, LPDIPROPHEADER) -> HRESULT override;
    auto STDMETHODCALLTYPE SetProperty(REFGUID, LPCDIPROPHEADER) -> HRESULT override;
    auto STDMETHODCALLTYPE Acquire() -> HRESULT override;
    auto STDMETHODCALLTYPE Unacquire() -> HRESULT override;
    auto STDMETHODCALLTYPE GetDeviceState(DWORD size, void* output) -> HRESULT override;
    auto STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT p) -> HRESULT override;
    auto STDMETHODCALLTYPE SetEventNotification(HANDLE) -> HRESULT override;
    auto STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD flags) -> HRESULT override;
    auto STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA p) -> HRESULT override;
    auto STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) -> HRESULT override;
    auto STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) -> HRESULT override;
    auto STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD p) -> HRESULT override;
    auto STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) -> HRESULT override;
    auto STDMETHODCALLTYPE Poll() -> HRESULT override;
    auto STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD count, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) -> HRESULT override;

private:
    void invalidateSample() noexcept;

    ULONG references_       = 1;
    HWND  window_           = nullptr;
    POINT previous_         = {};
    bool  mouseSampleValid_ = false;
    LONG  previousWidth_    = 0;
    LONG  previousHeight_   = 0;
    BYTE  previousButtons_  = 0;
};

class LocalInput final : public IDirectInput8A
{
public:
    LocalInput()                                                  = default;
    ~LocalInput()                                                 = default;
    LocalInput(const LocalInput&)                                 = delete;
    auto operator=(const LocalInput&) -> LocalInput&              = delete;
    LocalInput(LocalInput&&)                                      = delete;
    auto                   operator=(LocalInput&&) -> LocalInput& = delete;
    static auto            supports(REFIID iid) noexcept -> bool;
    auto STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) -> HRESULT override;
    auto STDMETHODCALLTYPE AddRef() -> ULONG override;
    auto STDMETHODCALLTYPE Release() -> ULONG override;
    auto STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) -> HRESULT override;
    auto STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE FindDevice(REFGUID, LPCSTR, LPGUID) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID) -> HRESULT override;

private:
    ULONG references_ = 1;
};

} // namespace tmloader
