#include "app/runtime.h"
#include "ui/window.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{

constexpr RECT  kContentRect{ 240, 0, 1680, 1080 };
constexpr RECT  kSourceRect{ 11, 21, 631, 471 };
constexpr DWORD kFakeShader        = 0x123;
constexpr DWORD kOtherVertexFormat = 0x142;
constexpr UINT  kBufferSize        = 128;
constexpr DWORD kBufferUsage       = 0x238;
// Present activates the game window on its first frame, which reads game memory.
constexpr DWORD kFramesPastActivation = 10;

bool     monitorAvailable = true;
bool     displayAvailable = true;
DWORD    displayWidth     = 3840;
DWORD    displayHeight    = 2160;
unsigned screenshotChecks;

auto fakeIconic(HWND) -> BOOL
{
    return FALSE;
}

auto fakeMonitor(HWND, DWORD) -> HMONITOR
{
    return reinterpret_cast<HMONITOR>(1);
}

auto fakeMonitorInfo(HMONITOR, MONITORINFOEXW*) -> BOOL
{
    return monitorAvailable;
}

auto fakeDisplaySettings(LPCWSTR, DWORD mode, DEVMODEW* info) -> BOOL
{
    REQUIRE(mode == ENUM_CURRENT_SETTINGS);
    info->dmPelsWidth  = displayWidth;
    info->dmPelsHeight = displayHeight;
    return displayAvailable;
}

} // namespace

#define IsIconic             fakeIconic
#define MonitorFromWindow    fakeMonitor
#define GetMonitorInfoW      fakeMonitorInfo
#define EnumDisplaySettingsW fakeDisplaySettings
#include "graphics/graphics.cpp"
#undef IsIconic

namespace tmloader
{

Config options;
BYTE*  gameBase{};
HWND   gameWindow = reinterpret_cast<HWND>(1);

void writeLog(std::string_view)
{
}

void setImport(HMODULE, DWORD, void*)
{
}

void initializeGameWindow(HWND)
{
}

auto currentWindowSettings() -> WindowSettings
{
    return {};
}

void requestScreenshot() noexcept
{
}

void saveRequestedScreenshot(void*) noexcept
{
    ++screenshotChecks;
}

auto gameContentRect() -> RECT
{
    return kContentRect;
}

} // namespace tmloader

namespace
{

DWORD selectedShader;
DWORD bufferFormat;
int   shaderCreates;
int   shaderDeletes;
int   constantsCalls;

auto WINAPI reset(void*, tmloader::d3d8::PresentParameters* pp) -> HRESULT
{
    const auto scale = tmloader::activeRenderScale();
    REQUIRE(pp->windowed);
    REQUIRE(pp->width == 640U * scale);
    REQUIRE(pp->height == 480U * scale);
    // D3D8 only accepts explicit Present rectangles with a single-buffer COPY swap chain.
    REQUIRE(pp->swap == tmloader::d3d8::SwapEffectCopy);
    REQUIRE(pp->count == 1);
    REQUIRE(pp->multisample == 0);
    REQUIRE(pp->window == tmloader::gameWindow);
    REQUIRE(pp->refresh == 0);
    return S_OK;
}

auto WINAPI present(void*, const RECT* source, const RECT* dest, HWND, const RGNDATA*) -> HRESULT
{
    REQUIRE(dest != nullptr);
    REQUIRE(dest->left == kContentRect.left);
    REQUIRE(dest->top == kContentRect.top);
    REQUIRE(dest->right == kContentRect.right);
    REQUIRE(dest->bottom == kContentRect.bottom);
    if (source)
    {
        const auto quarters = static_cast<int>(tmloader::activeRenderScale() * 4);
        REQUIRE(source->left == MulDiv(kSourceRect.left, quarters, 4));
        REQUIRE(source->top == MulDiv(kSourceRect.top, quarters, 4));
        REQUIRE(source->right == MulDiv(kSourceRect.right, quarters, 4));
        REQUIRE(source->bottom == MulDiv(kSourceRect.bottom, quarters, 4));
    }

    return S_OK;
}

auto WINAPI createShader(void*, const DWORD* declaration, const DWORD* code, DWORD* shader, DWORD) -> HRESULT
{
    REQUIRE(declaration[0] == 0x20000000);
    REQUIRE(code[0] == 0xfffe0101);
    *shader = kFakeShader;
    ++shaderCreates;
    return S_OK;
}

auto WINAPI deleteShader(void*, DWORD shader) -> HRESULT
{
    REQUIRE(shader == kFakeShader);
    ++shaderDeletes;
    return S_OK;
}

auto WINAPI constants(void*, DWORD index, const void* data, DWORD count) -> HRESULT
{
    REQUIRE(index == 0);
    REQUIRE(count == 2);
    const auto* const values = static_cast<const float*>(data);
    REQUIRE(std::abs(values[0] - 2.0f / 640) < 0.000001f);
    REQUIRE(values[3] == 1);
    ++constantsCalls;
    return S_OK;
}

auto WINAPI selectShader(void*, DWORD shader) -> HRESULT
{
    selectedShader = shader;
    return S_OK;
}

auto WINAPI makeBuffer(void*, UINT size, DWORD usage, DWORD format, DWORD pool, void**) -> HRESULT
{
    REQUIRE(size == kBufferSize);
    REQUIRE(usage == kBufferUsage);
    REQUIRE(pool == 0);
    bufferFormat = format;
    return S_OK;
}

// Answers only the vtable slots the scale shader helpers call.
struct FakeDevice
{
    std::array<void*, tmloader::d3d8::DeviceMethodCount> table{};
    void**                                               vtable = table.data();

    FakeDevice()
    {
        table[tmloader::d3d8::CreateVertexShader]      = reinterpret_cast<void*>(createShader);
        table[tmloader::d3d8::DeleteVertexShader]      = reinterpret_cast<void*>(deleteShader);
        table[tmloader::d3d8::SetVertexShaderConstant] = reinterpret_cast<void*>(constants);
    }

    FakeDevice(const FakeDevice&)                    = delete;
    auto operator=(const FakeDevice&) -> FakeDevice& = delete;
    FakeDevice(FakeDevice&&)                         = delete;
    auto operator=(FakeDevice&&) -> FakeDevice&      = delete;
    ~FakeDevice()                                    = default;

    auto get() -> void*
    {
        return &vtable;
    }
};

// Every case runs in its own process under CTest, so none may rely on another's hook state.
void installFakeDevice()
{
    using namespace tmloader;
    originalReset              = reset;
    originalPresent            = present;
    originalSetVertexShader    = selectShader;
    originalCreateVertexBuffer = makeBuffer;
    renderScale                = 1;
    scaleShader                = 0;
    frames                     = kFramesPastActivation;
    selectedShader             = 0;
    bufferFormat               = 0;
    shaderCreates              = 0;
    shaderDeletes              = 0;
    constantsCalls             = 0;
    screenshotChecks           = 0;
}

auto fullscreenParameters() -> tmloader::d3d8::PresentParameters
{
    auto pp    = tmloader::d3d8::PresentParameters{};
    pp.width   = 1920;
    pp.height  = 1080;
    pp.swap    = 1;
    pp.refresh = 60;
    return pp;
}

} // namespace

TEST_CASE("Automatic render scale follows the monitor's display mode", "[graphics]")
{
    monitorAvailable = true;
    displayAvailable = true;
    displayWidth     = 3840;
    displayHeight    = 2160;
    REQUIRE(tmloader::monitorRenderScale(nullptr, 640, 480) == 4.5f);

    displayWidth  = 1920;
    displayHeight = 1080;
    REQUIRE(tmloader::monitorRenderScale(nullptr, 3840, 2160) == 2.25f);
}

TEST_CASE("Automatic render scale falls back to the adapter mode", "[graphics]")
{
    displayWidth  = 1920;
    displayHeight = 1080;

    monitorAvailable = false;
    displayAvailable = true;
    REQUIRE(tmloader::monitorRenderScale(nullptr, 2560, 1440) == 3);

    monitorAvailable = true;
    displayAvailable = false;
    REQUIRE(tmloader::monitorRenderScale(nullptr, 1280, 720) == 1.5f);
    REQUIRE(tmloader::monitorRenderScale(nullptr, 0, 0) == 1);
}

TEST_CASE("A vtable that already holds our hooks keeps the recorded originals", "[graphics]")
{
    using namespace tmloader;
    installFakeDevice();
    void* entries[] = { reinterpret_cast<void*>(hookedPresent), reinterpret_cast<void*>(hookedReset) };
    hook(originalPresent, entries[0], hookedPresent);
    hook(originalReset, entries[1], hookedReset);
    // Parenthesised so Catch2 does not try to print __stdcall function pointers.
    REQUIRE((originalPresent == present));
    REQUIRE((originalReset == reset));
}

TEST_CASE("Reset turns fullscreen parameters into windowed ones", "[graphics]")
{
    using namespace tmloader;
    installFakeDevice();
    auto pp = fullscreenParameters();
    REQUIRE(hookedReset(nullptr, &pp) == S_OK);
}

TEST_CASE("Present draws into the content rectangle and checks for a screenshot request", "[graphics]")
{
    using namespace tmloader;
    installFakeDevice();
    REQUIRE(hookedPresent(nullptr, nullptr, nullptr, nullptr, nullptr) == S_OK);
    REQUIRE(screenshotChecks == 1);
}

TEST_CASE("Present scales the source rectangle to the back buffer", "[graphics]")
{
    using namespace tmloader;
    installFakeDevice();
    for (const float scale : kRenderScales)
    {
        renderScale       = scale;
        const auto source = kSourceRect;
        REQUIRE(hookedPresent(nullptr, &source, nullptr, nullptr, nullptr) == S_OK);
        REQUIRE(source.left == kSourceRect.left);
        REQUIRE(source.right == kSourceRect.right);
    }

    REQUIRE(screenshotChecks == kRenderScales.size());
}

TEST_CASE("The game vertex format selects the scale shader above 1x", "[graphics]")
{
    using namespace tmloader;
    installFakeDevice();
    FakeDevice device;
    auto       pp = fullscreenParameters();
    for (const float scale : kRenderScales)
    {
        renderScale = scale;
        REQUIRE(hookedReset(device.get(), &pp) == S_OK);
        const auto expectedShader = [&]() -> DWORD
        {
            if (scale == 1)
            {
                return kGameVertexFormat;
            }

            return kFakeShader;
        }();
        REQUIRE(hookedSetVertexShader(device.get(), kGameVertexFormat) == S_OK);
        REQUIRE(selectedShader == expectedShader);
        REQUIRE(hookedSetVertexShader(device.get(), kOtherVertexFormat) == S_OK);
        REQUIRE(selectedShader == kOtherVertexFormat);
    }

    deleteScaleShader(device.get(), scaleShader);
    REQUIRE(shaderCreates == static_cast<int>(kRenderScales.size()) - 1);
    REQUIRE(shaderDeletes == shaderCreates);
    REQUIRE(constantsCalls == shaderCreates);
}

TEST_CASE("Game vertex buffers lose their format tag above 1x", "[graphics]")
{
    using namespace tmloader;
    installFakeDevice();
    for (const float scale : kRenderScales)
    {
        renderScale               = scale;
        const auto expectedFormat = [&]() -> DWORD
        {
            if (scale == 1)
            {
                return kGameVertexFormat;
            }

            return 0;
        }();
        REQUIRE(hookedCreateVertexBuffer(nullptr, kBufferSize, kBufferUsage, kGameVertexFormat, 0, nullptr) == S_OK);
        REQUIRE(bufferFormat == expectedFormat);
        REQUIRE(hookedCreateVertexBuffer(nullptr, kBufferSize, kBufferUsage, kOtherVertexFormat, 0, nullptr) == S_OK);
        REQUIRE(bufferFormat == kOtherVertexFormat);
    }
}
