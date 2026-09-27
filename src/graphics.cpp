#include "runtime.h"
#include "window.h"
namespace tmloader {
// D3D8 ABI layout, independently declared to avoid requiring the obsolete SDK.
struct Present8
{
    DWORD width, height, format, count, multisample, swap;
    HWND window;
    BOOL windowed, depth;
    DWORD depthFormat, flags, refresh, interval;
};
typedef HRESULT(WINAPI *CreateDevice8)(void *, UINT, DWORD, HWND, DWORD, Present8 *, void **);
static CreateDevice8 originalDevice;
typedef HRESULT(WINAPI *PresentFn)(void *, const RECT *, const RECT *, HWND, const RGNDATA *);
static PresentFn originalPresent;
using ResetFn = HRESULT(WINAPI*)(void*, Present8*);
static ResetFn originalReset;
static DWORD desktopFormat = 22;

static void windowedParameters(Present8& pp) {
    pp.windowed = TRUE;
    pp.width = 640;
    pp.height = 480;
    pp.format = desktopFormat;
    pp.count = 1;
    pp.multisample = 0;
    pp.swap = 3; // D3DSWAPEFFECT_COPY permits a scaled destination rectangle.
    pp.refresh = 0;
    pp.interval = 0;
    pp.window = gameWindow;
    pp.flags |= 1;
}
static HRESULT WINAPI resetDevice(void* self, Present8* pp) {
    windowedParameters(*pp);
    HRESULT hr = originalReset(self, pp);
    logf("D3D8 windowed Reset=%08lX", hr);
    return hr;
}
static DWORD drawCalls, drawErrors, textureCalls;
typedef HRESULT(WINAPI *DrawFn)(void *, DWORD, UINT, UINT);
typedef HRESULT(WINAPI *DrawUPFn)(void *, DWORD, UINT, const void *, UINT);
typedef HRESULT(WINAPI *DrawIdxFn)(void *, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *DrawIdxUPFn)(void *, DWORD, UINT, UINT, UINT, const void *, DWORD, const void *, UINT);
typedef HRESULT(WINAPI *TextureFn)(void *, UINT, UINT, UINT, DWORD, DWORD, DWORD, void **);
static DrawFn realDraw;
static DrawUPFn realDrawUP;
static DrawIdxFn realDrawIdx;
static DrawIdxUPFn realDrawIdxUP;
static TextureFn realTexture;
static HRESULT countDraw(HRESULT hr)
{
    drawCalls++;
    if (FAILED(hr) && drawErrors++ < 6)
        logf("D3D8 draw failed %08lX", hr);
    return hr;
}
static HRESULT WINAPI draw(void *d, DWORD t, UINT s, UINT n)
{
    return countDraw(realDraw(d, t, s, n));
}
static HRESULT WINAPI drawUP(void *d, DWORD t, UINT n, const void *data, UINT stride)
{
    return countDraw(realDrawUP(d, t, n, data, stride));
}
static HRESULT WINAPI drawIdx(void *d, DWORD t, UINT m, UINT v, UINT s, UINT n)
{
    return countDraw(realDrawIdx(d, t, m, v, s, n));
}
static HRESULT WINAPI drawIdxUP(void *d, DWORD t, UINT m, UINT v, UINT n, const void *i, DWORD f, const void *p,
                                UINT stride)
{
    return countDraw(realDrawIdxUP(d, t, m, v, n, i, f, p, stride));
}
static HRESULT WINAPI texture(void *d, UINT w, UINT h, UINT l, DWORD u, DWORD f, DWORD pool, void **out)
{
    HRESULT hr = realTexture(d, w, h, l, u, f, pool, out);
    if (++textureCalls <= 8 || FAILED(hr))
        logf("D3D8 texture %ux%u format=%lu hr=%08lX", w, h, f, hr);
    return hr;
}
static void captureFrame(void *device, DWORD frame)
{
    logf("Render counters: draws=%lu errors=%lu textures=%lu", drawCalls, drawErrors, textureCalls);
    void **vt = *(void ***)device;
    void *surface = NULL;
    typedef HRESULT(WINAPI * Back)(void *, UINT, DWORD, void **);
    HRESULT hr = ((Back)vt[16])(device, 0, 0, &surface);
    if (FAILED(hr) || !surface)
        return;
    void **sv = *(void ***)surface;
    DWORD desc[8] = {};
    typedef HRESULT(WINAPI * Desc)(void *, DWORD *);
    ((Desc)sv[8])(surface, desc);
    struct Locked
    {
        INT pitch;
        void *pixels;
    } locked = {};
    typedef HRESULT(WINAPI * Lock)(void *, Locked *, const RECT *, DWORD);
    hr = ((Lock)sv[9])(surface, &locked, NULL, 0x10);
    if (SUCCEEDED(hr) && (desc[0] == 21 || desc[0] == 22))
    {
        char name[40];
        sprintf_s(name, "frame_%05lu.bmp", frame);
        const auto path = options.runtimeDir / "logs/captures" / name;
        FILE *f = outputPathAllowed(options, path) ? _wfopen(path.c_str(), L"wb") : nullptr;
        if (!f) logf("Diagnostic capture skipped: unsafe or unwritable destination");
        if (f)
        {
            BITMAPFILEHEADER bf = {};
            BITMAPINFOHEADER bi = {};
            bi.biSize = sizeof(bi);
            bi.biWidth = desc[6];
            bi.biHeight = -(LONG)desc[7];
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biSizeImage = desc[6] * desc[7] * 4;
            bf.bfType = 0x4d42;
            bf.bfOffBits = sizeof(bf) + sizeof(bi);
            bf.bfSize = bf.bfOffBits + bi.biSizeImage;
            fwrite(&bf, sizeof(bf), 1, f);
            fwrite(&bi, sizeof(bi), 1, f);
            for (DWORD y = 0; y < desc[7]; y++)
                fwrite((BYTE *)locked.pixels + y * locked.pitch, desc[6] * 4, 1, f);
            fclose(f);
            logf("Saved native rendered frame %lu: %s", frame, name);
        }
        typedef HRESULT(WINAPI * Unlock)(void *);
        ((Unlock)sv[10])(surface);
    }
    ((IUnknown *)surface)->Release();
}
static HRESULT WINAPI presentFrame(void *self, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty)
{
    DWORD frame = ++frames;
    if (frame == 1)
        logf("Initial graphics gate=%lu batch gate=%lu", gameValue(0x2ff22c), gameValue(0x2fe8ec));
    if (frame == 1)
    {
        ShowWindow(gameWindow, SW_SHOW);
        SetActiveWindow(gameWindow);
        SetFocus(gameWindow);
        BOOL active = SetForegroundWindow(gameWindow);
        logf("Activate game window=%p result=%d foreground=%p active=%p", gameWindow, active, GetForegroundWindow(),
             GetActiveWindow());
        auto* sound = reinterpret_cast<DWORD*>(gameValue(0x2ff164));
        logf("Native sound engine initialized=%lu", sound ? sound[1] : 0);
    }
    if (options.diagnostics && (frame == 60 || frame == 300 || frame == 600 || frame == 900 || frame == 1200 || frame == 1440 || frame == 1500 || frame == 1800 || (frame > 1800 && frame % 600 == 0)))
    {
        logf("TM state=%lu active=%lu task=%lu manager=%08lX", gameValue(0x2ff270), gameValue(0x2ff228),
             gameValue(0x306adc), gameValue(0x2ff1fc));
        captureFrame(self, frame);
    }
    if (IsIconic(gameWindow)) return originalPresent(self, source, dest, window, dirty);
    RECT target = gameContentRect();
    if (target.right <= target.left || target.bottom <= target.top) return S_OK;
    HRESULT hr = originalPresent(self, source, &target, window, dirty);
    static HRESULT lastResult = S_OK;
    if (FAILED(hr) && hr != lastResult) logf("D3D8 Present=%08lX", hr);
    lastResult = hr;
    return hr;
}
static HRESULT WINAPI windowedDevice(void *self, UINT adapter, DWORD type, HWND window, DWORD behavior, Present8 *pp,
                                     void **device)
{
    logf("D3D8 CreateDevice adapter=%u type=%lu behavior=%lX window=%p %lux%lu format=%lu windowed=%d", adapter, type,
         behavior, window, pp->width, pp->height, pp->format, pp->windowed);
    initializeGameWindow(window);
    gameValue(0x2ff248) = 1;
    DWORD mode[4] = {};
    typedef HRESULT(WINAPI * DisplayMode)(void *, UINT, DWORD *);
    void **vt = *(void ***)self;
    HRESULT modeHr = ((DisplayMode)vt[8])(self, adapter, mode);
    logf("D3D8 desktop mode=%08lX %lux%lu format=%lu", modeHr, mode[0], mode[1], mode[3]);
    desktopFormat = SUCCEEDED(modeHr) ? mode[3] : 22;
    windowedParameters(*pp);
    HRESULT hr = originalDevice(self, adapter, type, window, (behavior & ~(0x40 | 0x80 | 0x10)) | 0x20, pp, device);
    logf("D3D8 windowed CreateDevice=%08lX device=%p", hr, *device);
    if (SUCCEEDED(hr))
    {
        void **dv = *(void ***)*device;
        originalPresent = (PresentFn)dv[15];
        originalReset = (ResetFn)dv[14];
        realDraw = (DrawFn)dv[70];
        realDrawIdx = (DrawIdxFn)dv[71];
        realDrawUP = (DrawUPFn)dv[72];
        realDrawIdxUP = (DrawIdxUPFn)dv[73];
        realTexture = (TextureFn)dv[20];
        DWORD old;
        VirtualProtect(dv, 97 * 4, PAGE_READWRITE, &old);
        dv[14] = (void*)resetDevice;
        dv[15] = (void *)presentFrame;
        if (options.diagnostics) {
            dv[20] = (void *)texture;
            dv[70] = (void *)draw;
            dv[71] = (void *)drawIdx;
            dv[72] = (void *)drawUP;
            dv[73] = (void *)drawIdxUP;
        }
        VirtualProtect(dv, 97 * 4, old, &old);
    }
    return hr;
}
void *WINAPI localD3D8(UINT version)
{
    typedef void *(WINAPI * Factory)(UINT);
    Factory f = (Factory)GetProcAddress(LoadLibraryA("d3d8.dll"), "Direct3DCreate8");
    void *obj = f(version);
    logf("Direct3DCreate8 version=%u object=%p", version, obj);
    if (obj)
    {
        void **old = *(void ***)obj;
        void **vt = (void **)HeapAlloc(GetProcessHeap(), 0, 16 * sizeof(void *));
        memcpy(vt, old, 16 * sizeof(void *));
        originalDevice = (CreateDevice8)vt[15];
        vt[15] = (void *)windowedDevice;
        *(void ***)obj = vt;
    }
    return obj;
}

}
