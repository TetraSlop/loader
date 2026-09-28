#include "runtime.h"
#include "window.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

static auto fakeIconic(HWND) -> BOOL
{
    return FALSE;
}

static bool  monitorAvailable = true, displayAvailable = true;
static DWORD displayWidth = 3840, displayHeight = 2160;

static auto fakeMonitor(HWND, DWORD) -> HMONITOR
{
    return reinterpret_cast<HMONITOR>(1);
}

static auto fakeMonitorInfo(HMONITOR, MONITORINFOEXW*) -> BOOL
{
    return monitorAvailable;
}

static auto fakeDisplaySettings(LPCWSTR, DWORD mode, DEVMODEW* info) -> BOOL
{
    REQUIRE(mode == ENUM_CURRENT_SETTINGS);
    info->dmPelsWidth  = displayWidth;
    info->dmPelsHeight = displayHeight;
    return displayAvailable;
}

static unsigned screenshotChecks;
#define IsIconic             fakeIconic
#define MonitorFromWindow    fakeMonitor
#define GetMonitorInfoW      fakeMonitorInfo
#define EnumDisplaySettingsW fakeDisplaySettings
#include "../src/graphics.cpp"
#undef IsIconic

namespace tmloader
{

Config options;
BYTE*  gameBase{};
HWND   gameWindow = reinterpret_cast<HWND>(1);
DWORD  frames     = 10;

void logf(const char*, ...)
{
}

void initializeGameWindow(HWND)
{
}

auto currentWindowSettings() -> WindowSettings
{
    return {};
}

void saveRequestedScreenshot(void*) noexcept
{
    ++screenshotChecks;
}

auto gameContentRect() -> RECT
{
    return { 240, 0, 1680, 1080 };
}

} // namespace tmloader

static auto WINAPI reset(void*, tmloader::Present8* pp) -> HRESULT
{
    // Emulate D3D8's presentation contract: explicit rectangles require COPY.
    const float scale = tmloader::activeRenderScale();
    REQUIRE((pp->windowed && pp->width == 640U * scale && pp->height == 480U * scale));
    REQUIRE((pp->swap == 3 && pp->count == 1 && pp->multisample == 0));
    REQUIRE((pp->window == tmloader::gameWindow && pp->refresh == 0));
    return S_OK;
}

static auto WINAPI present(void*, const RECT* source, const RECT* dest, HWND, const RGNDATA*) -> HRESULT
{
    REQUIRE((dest && dest->left == 240 && dest->top == 0 && dest->right == 1680 && dest->bottom == 1080));
    if (source)
    {
        const int quarters = static_cast<int>(tmloader::activeRenderScale() * 4);
        REQUIRE((source->left == MulDiv(11, quarters, 4) && source->top == MulDiv(21, quarters, 4) &&
                 source->right == MulDiv(631, quarters, 4) && source->bottom == MulDiv(471, quarters, 4)));
    }
    return S_OK;
}

static DWORD selectedShader, bufferFormat;
static int   shaderCreates, shaderDeletes, constantsCalls;

static auto WINAPI createShader(void*, const DWORD* declaration, const DWORD* code, DWORD* shader, DWORD) -> HRESULT
{
    REQUIRE((declaration[0] == 0x20000000 && code[0] == 0xfffe0101));
    *shader = 0x123;
    ++shaderCreates;
    return S_OK;
}

static auto WINAPI deleteShader(void*, DWORD shader) -> HRESULT
{
    REQUIRE(shader == 0x123);
    ++shaderDeletes;
    return S_OK;
}

static auto WINAPI constants(void*, DWORD index, const void* data, DWORD count) -> HRESULT
{
    REQUIRE((index == 0 && count == 2));
    auto* const values = static_cast<const float*>(data);
    REQUIRE((std::abs(values[0] - 2.0f / 640) < 0.000001f && values[3] == 1));
    ++constantsCalls;
    return S_OK;
}

static auto WINAPI selectShader(void*, DWORD shader) -> HRESULT
{
    selectedShader = shader;
    return S_OK;
}

static auto WINAPI makeBuffer(void*, UINT size, DWORD usage, DWORD format, DWORD pool, void**) -> HRESULT
{
    REQUIRE((size == 128 && usage == 0x238 && pool == 0));
    bufferFormat = format;
    return S_OK;
}

TEST_CASE("Graphics hooks preserve presentation and device lifecycle", "[graphics]")
{
    using namespace tmloader;
    REQUIRE(monitorRenderScale(nullptr, 640, 480) == 4.5f);
    displayWidth  = 1920;
    displayHeight = 1080;
    REQUIRE(monitorRenderScale(nullptr, 3840, 2160) == 2.25f);
    monitorAvailable = false;
    REQUIRE(monitorRenderScale(nullptr, 2560, 1440) == 3);
    monitorAvailable = true;
    displayAvailable = false;
    REQUIRE(monitorRenderScale(nullptr, 1280, 720) == 1.5f);
    REQUIRE(monitorRenderScale(nullptr, 0, 0) == 1);
    Present8 pp{};
    pp.width        = 1920;
    pp.height       = 1080;
    pp.swap         = 1;
    pp.refresh      = 60;
    originalReset   = reset;
    originalPresent = present;
    rememberOriginal(originalPresent, reinterpret_cast<void*>(presentFrame), presentFrame);
    rememberOriginal(originalReset, reinterpret_cast<void*>(resetDevice), resetDevice);
    REQUIRE((originalPresent == present && originalReset == reset));
    REQUIRE(resetDevice(nullptr, &pp) == S_OK);
    REQUIRE(presentFrame(nullptr, nullptr, nullptr, nullptr, nullptr) == S_OK);
    void* table[97]{};
    table[75]            = reinterpret_cast<void*>(createShader);
    table[78]            = reinterpret_cast<void*>(deleteShader);
    table[79]            = reinterpret_cast<void*>(constants);
    void** device        = table;
    originalVertexShader = selectShader;
    originalVertexBuffer = makeBuffer;
    for (const float factor : renderScales)
    {
        renderingFactor = factor;
        REQUIRE(resetDevice(&device, &pp) == S_OK);
        REQUIRE(setVertexShader(&device, gameVertexFormat) == S_OK);
        REQUIRE(selectedShader == (factor == 1 ? gameVertexFormat : 0x123));
        REQUIRE(vertexBuffer(&device, 128, 0x238, gameVertexFormat, 0, nullptr) == S_OK);
        REQUIRE(bufferFormat == (factor == 1 ? gameVertexFormat : 0));
        REQUIRE((vertexBuffer(&device, 128, 0x238, 0x142, 0, nullptr) == S_OK && bufferFormat == 0x142));
        REQUIRE((setVertexShader(&device, 0x142) == S_OK && selectedShader == 0x142));
        const auto source = RECT{ 11, 21, 631, 471 };
        REQUIRE(presentFrame(nullptr, &source, nullptr, nullptr, nullptr) == S_OK);
        REQUIRE((source.left == 11 && source.right == 631));
    }
    deleteScaleShader(&device, scaleShader);
    REQUIRE((shaderCreates == static_cast<int>(renderScales.size()) - 1 && shaderDeletes == shaderCreates && constantsCalls == shaderCreates));
    REQUIRE(screenshotChecks == renderScales.size() + 1);
}
