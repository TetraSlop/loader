#pragma once
#include <windows.h>

#include <dinput.h>
#include <wrl/implements.h>

// dinput.h declares its interfaces without a uuid; WRL answers QueryInterface through __uuidof.
// NOLINTBEGIN(cppcoreguidelines-virtual-class-destructor)
struct __declspec(uuid("BF798030-483A-4DA2-AA99-5D64ED369700")) IDirectInput8A;
struct __declspec(uuid("54D41080-DC15-4833-A41B-748F73A38179")) IDirectInputDevice8A;

// NOLINTEND(cppcoreguidelines-virtual-class-destructor)

namespace tmloader
{

// Moves the game's cursor to an absolute 640x480 position through its relative
// motion (input/input.cpp); returns false before the game is running.
auto mapGameMouse(LONG x, LONG y, LONG* dx, LONG* dy) -> bool;

// DirectInput mouse backed by the Windows pointer, mapped into the 640x480 game area.
class LocalMouse final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDirectInputDevice8A>
{
public:
    // The game gets the device back for whatever interface it asks.
    auto STDMETHODCALLTYPE QueryInterface(REFIID, void** out) -> HRESULT override;
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

    HWND  window_           = nullptr;
    POINT previous_         = {};
    bool  mouseSampleValid_ = false;
    LONG  previousWidth_    = 0;
    LONG  previousHeight_   = 0;
    BYTE  previousButtons_  = 0;
};

class LocalInput final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDirectInput8A>
{
public:
    static auto            supports(REFIID iid) noexcept -> bool;
    auto STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) -> HRESULT override;
    auto STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE FindDevice(REFGUID, LPCSTR, LPGUID) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD) -> HRESULT override;
    auto STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID) -> HRESULT override;
};

} // namespace tmloader
