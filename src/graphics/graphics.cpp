#include "app/abi.h"
#include "app/hooks.h"
#include "app/log.h"
#include "graphics/d3d8_abi.h"
#include "graphics/render_scale.h"
#include "graphics/screenshot.h"
#include "platform/patch.h"
#include "ui/window.h"
#include <algorithm>
#include <atomic>

// Runs the fullscreen D3D8 renderer in a window, optionally above its fixed 640x480.
namespace tmloader
{
namespace
{

constexpr DWORD kDirect3DCreateImport = 0x204260;
constexpr DWORD kGraphicsGate         = 0x2ff22c;
constexpr DWORD kBatchGate            = 0x2fe8ec;
constexpr DWORD kSoundEngine          = 0x2ff164;
constexpr DWORD kGameState            = 0x2ff270;
constexpr DWORD kGameActive           = 0x2ff228;
constexpr DWORD kGameTask             = 0x306adc;
constexpr DWORD kGameManager          = 0x2ff1fc;

// D3DCREATE_PUREDEVICE, HARDWARE_ and MIXED_VERTEXPROCESSING; the scale shader needs software.
constexpr DWORD kHardwareVertexFlags = 0x10 | 0x40 | 0x80;

// Frames whose native state is logged in diagnostics mode.
constexpr DWORD kDiagnosticFrames[] = { 60, 300, 600, 900, 1200, 1440, 1500, 1800 };

using CreateDeviceFn       = HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, d3d8::PresentParameters*, void**);
using ResetFn              = HRESULT(WINAPI*)(void*, d3d8::PresentParameters*);
using PresentFn            = HRESULT(WINAPI*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetVertexShaderFn    = HRESULT(WINAPI*)(void*, DWORD);
using CreateVertexBufferFn = HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**);

CreateDeviceFn       originalCreateDevice;
ResetFn              originalReset;
PresentFn            originalPresent;
SetVertexShaderFn    originalSetVertexShader;
CreateVertexBufferFn originalCreateVertexBuffer;

std::atomic<float> renderScale{ 1 };
DWORD              desktopFormat = d3d8::FormatX8R8G8B8;
DWORD              scaleShader   = 0;
DWORD              frames        = 0;

auto WINAPI hookedCreateVertexBuffer(void* device, UINT size, DWORD usage, DWORD format, DWORD pool, void** output) -> HRESULT
{
    // D3D8 skips vertex shaders for XYZRHW-tagged buffers, so the scale shader needs untyped ones.
    if (renderScale > 1 && format == kGameVertexFormat)
    {
        format = 0;
    }

    return originalCreateVertexBuffer(device, size, usage, format, pool, output);
}

auto WINAPI hookedSetVertexShader(void* device, DWORD shader) -> HRESULT
{
    if (shader != kGameVertexFormat || renderScale <= 1)
    {
        return originalSetVertexShader(device, shader);
    }

    const auto hr = setScaleConstants(device, renderScale);
    if (FAILED(hr))
    {
        return hr;
    }

    return originalSetVertexShader(device, scaleShader);
}

void windowedParameters(d3d8::PresentParameters& parameters)
{
    parameters.windowed    = TRUE;
    parameters.width       = static_cast<UINT>(640 * renderScale);
    parameters.height      = static_cast<UINT>(480 * renderScale);
    parameters.format      = desktopFormat;
    parameters.count       = 1;
    parameters.multisample = 0;
    // Copy swaps allow presenting into an explicit destination rectangle.
    parameters.swap     = d3d8::SwapEffectCopy;
    parameters.refresh  = 0;
    parameters.interval = 0;
    parameters.window   = gameWindow;
    parameters.flags |= d3d8::kLockableBackBuffer;
}

auto WINAPI hookedReset(void* device, d3d8::PresentParameters* parameters) -> HRESULT
{
    deleteScaleShader(device, scaleShader);
    windowedParameters(*parameters);
    auto hr = originalReset(device, parameters);
    if (SUCCEEDED(hr) && renderScale > 1)
    {
        hr = createScaleShader(device, &scaleShader);
    }

    logLine("D3D8 windowed Reset={:08X}", hex(hr));
    return hr;
}

auto gameValue(DWORD rva) -> DWORD&
{
    return *reinterpret_cast<DWORD*>(gameBase + rva);
}

auto diagnosticFrame(DWORD frame) -> bool
{
    return std::ranges::contains(kDiagnosticFrames, frame) || (frame > 1800 && frame % 600 == 0);
}

void activateGameWindow()
{
    logLine("Initial graphics gate={} batch gate={}", gameValue(kGraphicsGate), gameValue(kBatchGate));
    ShowWindow(gameWindow, SW_SHOW);
    SetActiveWindow(gameWindow);
    SetFocus(gameWindow);
    const auto active = SetForegroundWindow(gameWindow);
    logLine("Activate game window={} result={} foreground={} active={}", address(gameWindow), active, address(GetForegroundWindow()), address(GetActiveWindow()));
    if (const auto* const sound = reinterpret_cast<const DWORD*>(gameValue(kSoundEngine)))
    {
        logLine("Native sound engine initialized={}", sound[1]);
    }
}

auto WINAPI hookedPresent(void* device, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty) -> HRESULT
{
    const auto frame = ++frames;
    if (frame == 1)
    {
        activateGameWindow();
    }

    if (options.diagnostics && diagnosticFrame(frame))
    {
        logLine("TM state={} active={} task={} manager={:08X}", gameValue(kGameState), gameValue(kGameActive), gameValue(kGameTask), gameValue(kGameManager));
        requestScreenshot();
    }

    saveRequestedScreenshot(device);
    auto scaledSource = RECT{};
    if (renderScale > 1)
    {
        if (source)
        {
            // Quarter steps cover every preset, including 2.25x (1080p).
            const auto quarters = static_cast<int>(renderScale * 4);
            scaledSource        = { MulDiv(source->left, quarters, 4), MulDiv(source->top, quarters, 4), MulDiv(source->right, quarters, 4), MulDiv(source->bottom, quarters, 4) };
            source              = &scaledSource;
        }

        dirty = nullptr;
    }

    if (IsIconic(gameWindow))
    {
        return originalPresent(device, source, destination, window, dirty);
    }

    const auto target = gameContentRect();
    if (target.right <= target.left || target.bottom <= target.top)
    {
        return S_OK;
    }

    const auto     hr         = originalPresent(device, source, &target, window, dirty);
    static HRESULT lastResult = S_OK;
    if (FAILED(hr) && hr != lastResult)
    {
        logLine("D3D8 Present={:08X}", hex(hr));
    }

    lastResult = hr;
    return hr;
}

// Display settings report physical pixels regardless of Windows DPI scaling.
auto monitorRenderScale(HWND window, UINT fallbackWidth, UINT fallbackHeight) -> float
{
    auto monitor   = MONITORINFOEXW{};
    monitor.cbSize = sizeof(monitor);
    auto mode      = DEVMODEW{};
    mode.dmSize    = sizeof(mode);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    {
        return automaticRenderScale(mode.dmPelsWidth, mode.dmPelsHeight);
    }

    return automaticRenderScale(fallbackWidth, fallbackHeight);
}

void hookDevice(void* device)
{
    auto* const vtable     = *static_cast<void***>(device);
    auto        protection = DWORD{};
    VirtualProtect(vtable, d3d8::DeviceMethodCount * sizeof(void*), PAGE_READWRITE, &protection);
    hook(originalReset, vtable[d3d8::Reset], hookedReset);
    hook(originalPresent, vtable[d3d8::Present], hookedPresent);
    hook(originalCreateVertexBuffer, vtable[d3d8::CreateVertexBuffer], hookedCreateVertexBuffer);
    hook(originalSetVertexShader, vtable[d3d8::SetVertexShader], hookedSetVertexShader);
    VirtualProtect(vtable, d3d8::DeviceMethodCount * sizeof(void*), protection, &protection);
}

auto WINAPI hookedCreateDevice(void* d3d, UINT adapter, DWORD type, HWND window, DWORD behavior, d3d8::PresentParameters* parameters, void** device) -> HRESULT
{
    logLine("D3D8 CreateDevice adapter={} type={} behavior={:X} window={} {}x{} format={} windowed={}", adapter, type, behavior, address(window), parameters->width, parameters->height, parameters->format, parameters->windowed);
    initializeGameWindow(window);
    gameValue(abi::kWindowedFlag) = 1;

    auto       mode   = d3d8::DisplayMode{};
    const auto modeHr = callMethod(d3d, d3d8::GetAdapterDisplayMode, adapter, &mode);
    logLine("D3D8 desktop mode={:08X} {}x{} format={}", hex(modeHr), mode.width, mode.height, mode.format);
    if (SUCCEEDED(modeHr))
    {
        desktopFormat = mode.format;
    }

    const auto settings = currentWindowSettings();
    if (settings.autoResolution)
    {
        renderScale = monitorRenderScale(window, mode.width, mode.height);
    }
    else
    {
        renderScale = settings.renderScale;
    }

    const auto create = [&]() -> HRESULT
    {
        windowedParameters(*parameters);
        return originalCreateDevice(d3d, adapter, type, window, (behavior & ~kHardwareVertexFlags) | d3d8::kSoftwareVertexProcessing, parameters, device);
    };
    auto hr = create();
    if (SUCCEEDED(hr) && renderScale > 1)
    {
        hr = createScaleShader(*device, &scaleShader);
        if (FAILED(hr))
        {
            static_cast<IUnknown*>(*device)->Release();
            *device = nullptr;
        }
    }

    if (FAILED(hr) && renderScale > 1)
    {
        logLine("Higher-resolution device/shader failed: {:08X}; using original resolution", hex(hr));
        renderScale = 1;
        hr          = create();
    }

    logLine("D3D8 windowed CreateDevice={:08X} device={}", hex(hr), *device);
    if (SUCCEEDED(hr))
    {
        hookDevice(*device);
        logLine("Rendering resolution: {}x{} ({}x)", parameters->width, parameters->height, activeRenderScale());
    }

    return hr;
}

// Returns an IDirect3D8 whose private vtable copy routes CreateDevice through us.
auto WINAPI hookedDirect3DCreate8(UINT version) -> void*
{
    using Factory       = void*(WINAPI*)(UINT);
    const auto  factory = reinterpret_cast<Factory>(GetProcAddress(LoadLibraryA("d3d8.dll"), "Direct3DCreate8"));
    auto* const d3d     = factory(version);
    logLine("Direct3DCreate8 version={} object={}", version, d3d);
    if (!d3d)
    {
        return nullptr;
    }

    // Intentionally leaked: the vtable must outlive every IDirect3D8 reference.
    auto* const vtable = new void*[d3d8::Direct3DMethodCount];
    std::copy_n(*static_cast<void***>(d3d), d3d8::Direct3DMethodCount, vtable);
    originalCreateDevice       = reinterpret_cast<CreateDeviceFn>(vtable[d3d8::CreateDevice]);
    vtable[d3d8::CreateDevice] = reinterpret_cast<void*>(hookedCreateDevice);
    *static_cast<void***>(d3d) = vtable;
    return d3d;
}

} // namespace

auto activeRenderScale() -> float
{
    return renderScale;
}

void installGraphicsHooks(HMODULE game)
{
    setImport(game, kDirect3DCreateImport, reinterpret_cast<void*>(hookedDirect3DCreate8));
}

} // namespace tmloader
