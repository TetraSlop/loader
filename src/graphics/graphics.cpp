#include "app/runtime.h"
#include "graphics/d3d8_abi.h"
#include "graphics/render_scale.h"
#include "graphics/screenshot.h"
#include "textures/texture_replacements.h"
#include "ui/window.h"
#include <atomic>

namespace tmloader
{
namespace
{

using CreateDevice8 = HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, Present8*, void**);
CreateDevice8 originalDevice;
using PresentFn = HRESULT(WINAPI*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFn originalPresent;
using ResetFn = HRESULT(WINAPI*)(void*, Present8*);
ResetFn            originalReset;
DWORD              desktopFormat = 22;
std::atomic<float> renderingFactor{ 1 };
DWORD              scaleShader;
using SetVertexShaderFn = HRESULT(WINAPI*)(void*, DWORD);
SetVertexShaderFn originalVertexShader;
using VertexBufferFn = HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**);
VertexBufferFn originalVertexBuffer;

auto WINAPI vertexBuffer(void* device, UINT size, DWORD usage, DWORD format, DWORD pool, void** output) -> HRESULT
{
    // D3D8 treats an XYZRHW-tagged buffer as already transformed and bypasses
    // shader input preparation. Use an untyped buffer for our explicit layout.
    if (renderingFactor > 1 && format == gameVertexFormat)
        format = 0;
    return originalVertexBuffer(device, size, usage, format, pool, output);
}

auto WINAPI setVertexShader(void* device, DWORD format) -> HRESULT
{
    if (format == gameVertexFormat && renderingFactor > 1)
    {
        const auto hr = setScaleConstants(device, renderingFactor);
        if (FAILED(hr))
            return hr;
        return originalVertexShader(device, scaleShader);
    }
    return originalVertexShader(device, format);
}

void windowedParameters(Present8& pp)
{
    pp.windowed    = TRUE;
    pp.width       = static_cast<UINT>(640 * renderingFactor);
    pp.height      = static_cast<UINT>(480 * renderingFactor);
    pp.format      = desktopFormat;
    pp.count       = 1;
    pp.multisample = 0;
    pp.swap        = 3; // D3DSWAPEFFECT_COPY permits a scaled destination rectangle.
    pp.refresh     = 0;
    pp.interval    = 0;
    pp.window      = gameWindow;
    pp.flags |= 1;
}

auto WINAPI resetDevice(void* self, Present8* pp) -> HRESULT
{
    deleteScaleShader(self, scaleShader);
    windowedParameters(*pp);
    HRESULT hr = originalReset(self, pp);
    if (SUCCEEDED(hr) && renderingFactor > 1)
        hr = createScaleShader(self, &scaleShader);
    logf("D3D8 windowed Reset=%08lX", hr);
    return hr;
}

DWORD drawCalls, drawErrors, textureCalls;
using DrawFn      = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT);
using DrawUPFn    = HRESULT(WINAPI*)(void*, DWORD, UINT, const void*, UINT);
using DrawIdxFn   = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT, UINT, UINT);
using DrawIdxUPFn = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT, UINT, const void*, DWORD, const void*, UINT);
using TextureFn   = HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
DrawFn      realDraw;
DrawUPFn    realDrawUP;
DrawIdxFn   realDrawIdx;
DrawIdxUPFn realDrawIdxUP;
TextureFn   realTexture;

template <class T>
void rememberOriginal(T& destination, void* entry, T wrapper)
{
    // Some D3D8 implementations share method tables between device instances.
    // A later native device creation must not record our own hook as its target.
    if (entry != reinterpret_cast<void*>(wrapper))
        destination = reinterpret_cast<T>(entry);
}

auto countDraw(HRESULT hr) -> HRESULT
{
    drawCalls++;
    if (FAILED(hr) && drawErrors++ < 6)
        logf("D3D8 draw failed %08lX", hr);
    return hr;
}

auto WINAPI draw(void* d, DWORD t, UINT s, UINT n) -> HRESULT
{
    const auto               settings = currentWindowSettings();
    const TextureDrawBinding replacement(d, settings.hdTextures, settings.hdFonts);
    return countDraw(realDraw(d, t, s, n));
}

auto WINAPI drawUP(void* d, DWORD t, UINT n, const void* data, UINT stride) -> HRESULT
{
    const auto               settings = currentWindowSettings();
    const TextureDrawBinding replacement(d, settings.hdTextures, settings.hdFonts);
    return countDraw(realDrawUP(d, t, n, data, stride));
}

auto WINAPI drawIdx(void* d, DWORD t, UINT m, UINT v, UINT s, UINT n) -> HRESULT
{
    const auto               settings = currentWindowSettings();
    const TextureDrawBinding replacement(d, settings.hdTextures, settings.hdFonts);
    return countDraw(realDrawIdx(d, t, m, v, s, n));
}

auto WINAPI drawIdxUP(void* d, DWORD t, UINT m, UINT v, UINT n, const void* i, DWORD f, const void* p, UINT stride) -> HRESULT
{
    const auto               settings = currentWindowSettings();
    const TextureDrawBinding replacement(d, settings.hdTextures, settings.hdFonts);
    return countDraw(realDrawIdxUP(d, t, m, v, n, i, f, p, stride));
}

auto WINAPI texture(void* d, UINT w, UINT h, UINT l, DWORD u, DWORD f, DWORD pool, void** out) -> HRESULT
{
    const auto hr = realTexture(d, w, h, l, u, f, pool, out);
    if (++textureCalls <= 8 || FAILED(hr))
        logf("D3D8 texture %ux%u format=%lu hr=%08lX", w, h, f, hr);
    return hr;
}

void captureFrame(void* device, DWORD frame)
{
    logf("Render counters: draws=%lu errors=%lu textures=%lu", drawCalls, drawErrors, textureCalls);
    auto* const vt      = *static_cast<void***>(device);
    void*       surface = nullptr;
    using Back          = HRESULT(WINAPI*)(void*, UINT, DWORD, void**);
    HRESULT hr          = reinterpret_cast<Back>(vt[16])(device, 0, 0, &surface);
    if (FAILED(hr) || !surface)
        return;
    auto* const sv      = *static_cast<void***>(surface);
    DWORD       desc[8] = {};
    using Desc          = HRESULT(WINAPI*)(void*, DWORD*);
    reinterpret_cast<Desc>(sv[8])(surface, desc);

    struct Locked
    {
        INT   pitch;
        void* pixels;
    } locked = {};

    using Lock = HRESULT(WINAPI*)(void*, Locked*, const RECT*, DWORD);
    hr         = reinterpret_cast<Lock>(sv[9])(surface, &locked, nullptr, 0x10);
    if (SUCCEEDED(hr) && (desc[0] == 21 || desc[0] == 22))
    {
        char name[40];
        sprintf_s(name, "frame_%05lu.bmp", frame);
        const auto  path = options.runtimeDir / "logs/captures" / name;
        FILE* const f    = outputPathAllowed(options, path) ? _wfopen(path.c_str(), L"wb") : nullptr;
        if (!f)
            logf("Diagnostic capture skipped: unsafe or unwritable destination");
        if (f)
        {
            BITMAPFILEHEADER bf = {};
            BITMAPINFOHEADER bi = {};
            bi.biSize           = sizeof(bi);
            bi.biWidth          = desc[6];
            bi.biHeight         = -static_cast<LONG>(desc[7]);
            bi.biPlanes         = 1;
            bi.biBitCount       = 32;
            bi.biSizeImage      = desc[6] * desc[7] * 4;
            bf.bfType           = 0x4d42;
            bf.bfOffBits        = sizeof(bf) + sizeof(bi);
            bf.bfSize           = bf.bfOffBits + bi.biSizeImage;
            fwrite(&bf, sizeof(bf), 1, f);
            fwrite(&bi, sizeof(bi), 1, f);
            for (DWORD y = 0; y < desc[7]; y++)
                fwrite(static_cast<const BYTE*>(locked.pixels) + y * locked.pitch, desc[6] * 4, 1, f);
            fclose(f);
            logf("Saved native rendered frame %lu: %s", frame, name);
        }
        using Unlock = HRESULT(WINAPI*)(void*);
        reinterpret_cast<Unlock>(sv[10])(surface);
    }
    static_cast<IUnknown*>(surface)->Release();
}

auto WINAPI presentFrame(void* self, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) -> HRESULT
{
    const auto frame = ++frames;
    if (frame == 1)
        logf("Initial graphics gate=%lu batch gate=%lu", gameValue(0x2ff22c), gameValue(0x2fe8ec));
    if (frame == 1)
    {
        ShowWindow(gameWindow, SW_SHOW);
        SetActiveWindow(gameWindow);
        SetFocus(gameWindow);
        const auto active = SetForegroundWindow(gameWindow);
        logf("Activate game window=%p result=%d foreground=%p active=%p", gameWindow, active, GetForegroundWindow(), GetActiveWindow());
        auto* const sound = reinterpret_cast<DWORD*>(gameValue(0x2ff164));
        logf("Native sound engine initialized=%lu", sound ? sound[1] : 0);
    }
    if (options.diagnostics && (frame == 60 || frame == 300 || frame == 600 || frame == 900 || frame == 1200 || frame == 1440 || frame == 1500 || frame == 1800 || (frame > 1800 && frame % 600 == 0)))
    {
        logf("TM state=%lu active=%lu task=%lu manager=%08lX", gameValue(0x2ff270), gameValue(0x2ff228), gameValue(0x306adc), gameValue(0x2ff1fc));
        captureFrame(self, frame);
    }
    saveRequestedScreenshot(self);
    RECT scaledSource{};
    if (source && renderingFactor > 1)
    {
        // Quarter-step presets include native 1080p height at 2.25x.
        const auto quarters = static_cast<int>(renderingFactor * 4);
        scaledSource        = { MulDiv(source->left, quarters, 4), MulDiv(source->top, quarters, 4), MulDiv(source->right, quarters, 4), MulDiv(source->bottom, quarters, 4) };
        source              = &scaledSource;
    }
    if (renderingFactor > 1)
        dirty = nullptr; // Native Present uses a complete frame.
    if (IsIconic(gameWindow))
        return originalPresent(self, source, dest, window, dirty);
    const auto target = gameContentRect();
    if (target.right <= target.left || target.bottom <= target.top)
        return S_OK;
    const auto     hr         = originalPresent(self, source, &target, window, dirty);
    static HRESULT lastResult = S_OK;
    if (FAILED(hr) && hr != lastResult)
        logf("D3D8 Present=%08lX", hr);
    lastResult = hr;
    return hr;
}

auto monitorRenderScale(HWND window, UINT fallbackWidth, UINT fallbackHeight) -> float
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    // Display settings report physical pixels, including when Windows scaling
    // would otherwise virtualize window dimensions.
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        return automaticRenderScale(mode.dmPelsWidth, mode.dmPelsHeight);
    return automaticRenderScale(fallbackWidth, fallbackHeight);
}

auto WINAPI windowedDevice(void* self, UINT adapter, DWORD type, HWND window, DWORD behavior, Present8* pp, void** device) -> HRESULT
{
    logf("D3D8 CreateDevice adapter=%u type=%lu behavior=%lX window=%p %lux%lu format=%lu windowed=%d", adapter, type, behavior, window, pp->width, pp->height, pp->format, pp->windowed);
    initializeGameWindow(window);
    gameValue(0x2ff248) = 1;
    DWORD mode[4]       = {};
    using DisplayMode   = HRESULT(WINAPI*)(void*, UINT, DWORD*);
    auto* const vt      = *static_cast<void***>(self);
    const auto  modeHr  = reinterpret_cast<DisplayMode>(vt[8])(self, adapter, mode);
    logf("D3D8 desktop mode=%08lX %lux%lu format=%lu", modeHr, mode[0], mode[1], mode[3]);
    desktopFormat       = SUCCEEDED(modeHr) ? mode[3] : 22;
    const auto settings = currentWindowSettings();
    renderingFactor     = settings.autoResolution ? monitorRenderScale(window, mode[0], mode[1]) : settings.renderScale;
    windowedParameters(*pp);
    HRESULT hr = originalDevice(self, adapter, type, window, (behavior & ~(0x40 | 0x80 | 0x10)) | 0x20, pp, device);
    if (SUCCEEDED(hr) && renderingFactor > 1)
    {
        hr = createScaleShader(*device, &scaleShader);
        if (FAILED(hr))
        {
            reinterpret_cast<IUnknown*>(*device)->Release();
            *device = nullptr;
        }
    }
    if (FAILED(hr) && renderingFactor > 1)
    {
        logf("Higher-resolution device/shader failed: %08lX; using original resolution", hr);
        renderingFactor = 1;
        windowedParameters(*pp);
        hr = originalDevice(self, adapter, type, window, (behavior & ~(0x40 | 0x80 | 0x10)) | 0x20, pp, device);
    }
    logf("D3D8 windowed CreateDevice=%08lX device=%p", hr, *device);
    if (SUCCEEDED(hr))
    {
        auto* const dv = *static_cast<void***>(*device);
        rememberOriginal(originalPresent, dv[15], presentFrame);
        rememberOriginal(originalReset, dv[14], resetDevice);
        rememberOriginal(originalVertexShader, dv[76], setVertexShader);
        rememberOriginal(originalVertexBuffer, dv[23], vertexBuffer);
        rememberOriginal(realDraw, dv[70], draw);
        rememberOriginal(realDrawIdx, dv[71], drawIdx);
        rememberOriginal(realDrawUP, dv[72], drawUP);
        rememberOriginal(realDrawIdxUP, dv[73], drawIdxUP);
        rememberOriginal(realTexture, dv[20], texture);
        DWORD old;
        VirtualProtect(dv, 97 * 4, PAGE_READWRITE, &old);
        dv[14] = reinterpret_cast<void*>(resetDevice);
        dv[15] = reinterpret_cast<void*>(presentFrame);
        dv[76] = reinterpret_cast<void*>(setVertexShader);
        dv[23] = reinterpret_cast<void*>(vertexBuffer);
        if (options.diagnostics)
            dv[20] = reinterpret_cast<void*>(texture);
        if (options.diagnostics || texturePackCount())
        {
            dv[70] = reinterpret_cast<void*>(draw);
            dv[71] = reinterpret_cast<void*>(drawIdx);
            dv[72] = reinterpret_cast<void*>(drawUP);
            dv[73] = reinterpret_cast<void*>(drawIdxUP);
        }
        VirtualProtect(dv, 97 * 4, old, &old);
        logf("Rendering resolution: %lux%lu (%gx)", pp->width, pp->height, activeRenderScale());
    }
    return hr;
}

} // namespace

auto activeRenderScale() -> float
{
    return renderingFactor;
}

auto WINAPI localD3D8(UINT version) -> void*
{
    using Factory       = void*(WINAPI*)(UINT);
    const auto  factory = reinterpret_cast<Factory>(GetProcAddress(LoadLibraryA("d3d8.dll"), "Direct3DCreate8"));
    auto* const obj     = factory(version);
    logf("Direct3DCreate8 version=%u object=%p", version, obj);
    if (obj)
    {
        auto* const old = *static_cast<void***>(obj);
        auto* const vt  = static_cast<void**>(HeapAlloc(GetProcessHeap(), 0, 16 * sizeof(void*)));
        memcpy(vt, old, 16 * sizeof(void*));
        originalDevice             = reinterpret_cast<CreateDevice8>(vt[15]);
        vt[15]                     = reinterpret_cast<void*>(windowedDevice);
        *static_cast<void***>(obj) = vt;
    }
    return obj;
}

} // namespace tmloader
