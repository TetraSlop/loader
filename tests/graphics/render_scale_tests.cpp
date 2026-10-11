#include "app/runtime.h"
#include "graphics/d3d8_abi.h"
#include "graphics/render_scale.h"
#include "ui/window_settings.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <unknwn.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace tmloader;
using tmloader::callMethod;

namespace
{

constexpr UINT  kSdkVersion  = 220;
constexpr DWORD kClearColor  = 0xff123456;
constexpr DWORD kBufferUsage = 0x238;
// Largest per-channel difference that still counts as the same pixel.
constexpr unsigned kChannelTolerance = 2;

void check(const HRESULT hr)
{
    INFO("D3D8 HRESULT: " << hr);
    REQUIRE(SUCCEEDED(hr));
}

struct Vertex
{
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    DWORD specular;
    float u;
    float v;
};

static_assert(sizeof(Vertex) == 32);

auto pixels(void* device, const float factor) -> std::vector<DWORD>
{
    ComPtr<IUnknown> surface;
    check(callMethod(device, d3d8::GetBackBuffer, 0, 0, surface.GetAddressOf()));
    auto desc = d3d8::SurfaceDesc{};
    check(callMethod(surface.Get(), d3d8::SurfaceGetDesc, &desc));
    REQUIRE(desc.width == 640U * factor);
    REQUIRE(desc.height == 480U * factor);
    auto locked = d3d8::LockedRect{};
    check(callMethod(surface.Get(), d3d8::SurfaceLockRect, &locked, nullptr, d3d8::kLockReadOnly));
    auto result = std::vector<DWORD>(desc.width * desc.height);
    for (DWORD y = 0; y < desc.height; ++y)
    {
        std::memcpy(result.data() + y * desc.width, locked.bits + y * locked.pitch, desc.width * 4);
    }

    check(callMethod(surface.Get(), d3d8::SurfaceUnlock));
    return result;
}

auto render(void* device, void* buffer, const DWORD shader, const float factor, const bool reference) -> std::vector<DWORD>
{
    // A clipped quad with unequal W, interpolated alpha/color and noninteger
    // positions. CPU-scaled fixed-function rendering is the independent oracle.
    auto vertices = std::array<Vertex, 4>{ { { -12.5f, 15.75f, 0.25f, 0.5f, 0xffcc88aa, 0, 0, 0 },
                                             { 602.5f, 45.25f, 0.5f, 1.0f, 0xc088aaff, 0, 1, 0 },
                                             { -4.5f, 470.75f, 0.25f, 0.75f, 0x8888ffaa, 0, 0, 1 },
                                             { 630.5f, 491.25f, 0.75f, 0.25f, 0xffffaacc, 0, 1, 1 } } };
    if (reference)
    {
        for (auto& v : vertices)
        {
            v.x = (v.x + 0.5f) * factor - 0.5f;
            v.y = (v.y + 0.5f) * factor - 0.5f;
        }
    }

    void* data = nullptr;
    check(callMethod(buffer, d3d8::BufferLock, 0, sizeof(vertices), &data, d3d8::kLockDiscard));
    std::memcpy(data, vertices.data(), sizeof(vertices));
    check(callMethod(buffer, d3d8::BufferUnlock));
    check(callMethod(device, d3d8::SetStreamSource, 0, buffer, sizeof(Vertex)));
    if (reference)
    {
        check(callMethod(device, d3d8::SetVertexShader, kGameVertexFormat));
    }
    else
    {
        check(setScaleConstants(device, factor));
        check(callMethod(device, d3d8::SetVertexShader, shader));
    }

    check(callMethod(device, d3d8::Clear, 0, nullptr, 1, kClearColor, 1.0f, 0));
    check(callMethod(device, d3d8::BeginScene));
    check(callMethod(device, d3d8::DrawPrimitive, d3d8::kTriangleStrip, 0, 2));
    check(callMethod(device, d3d8::EndScene));
    return pixels(device, factor);
}

auto openDirect3D() -> ComPtr<IUnknown>
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    REQUIRE(tmloader::lowerIntegrity());
    // No window is created and no Present, focus or input API is called.
    const auto module  = LoadLibraryW(L"d3d8.dll");
    using Factory      = IUnknown*(WINAPI*)(UINT);
    const auto factory = reinterpret_cast<Factory>(GetProcAddress(module, "Direct3DCreate8"));
    if (!factory)
    {
        SKIP("Direct3D 8 unavailable");
    }

    ComPtr<IUnknown> d3d;
    d3d.Attach(factory(kSdkVersion));
    if (!d3d)
    {
        SKIP("Direct3D 8 unavailable");
    }

    return d3d;
}

auto createDevice(IUnknown* d3d, const DWORD format, const float factor) -> ComPtr<IUnknown>
{
    auto pp     = d3d8::PresentParameters{};
    pp.width    = static_cast<UINT>(640 * factor);
    pp.height   = static_cast<UINT>(480 * factor);
    pp.format   = format;
    pp.count    = 1;
    pp.swap     = d3d8::kSwapEffectCopy;
    pp.window   = GetDesktopWindow();
    pp.windowed = TRUE;
    pp.flags    = 1;
    ComPtr<IUnknown> device;
    check(callMethod(d3d, d3d8::CreateDevice, 0, d3d8::kDeviceTypeHal, pp.window, d3d8::kSoftwareVertexProcessing, &pp, device.GetAddressOf()));
    return device;
}

auto createCheckerTexture(void* device) -> ComPtr<IUnknown>
{
    ComPtr<IUnknown> texture;
    check(callMethod(device, d3d8::CreateTexture, 8, 8, 1, 0, d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged, texture.GetAddressOf()));
    auto locked = d3d8::LockedRect{};
    check(callMethod(texture.Get(), d3d8::TextureLockRect, 0, &locked, nullptr, 0));
    for (int y = 0; y < 8; ++y)
    {
        for (int x = 0; x < 8; ++x)
        {
            reinterpret_cast<DWORD*>(locked.bits + y * locked.pitch)[x] = [&]() -> DWORD
            {
                if ((x + y) % 2)
                {
                    return 0xffdddddd;
                }

                return 0xff336699;
            }();
        }
    }

    check(callMethod(texture.Get(), d3d8::TextureUnlock, 0));
    return texture;
}

void applyGameStates(void* device)
{
    // Lighting, clipping, cull, z, alpha blend, src/dest blend.
    for (const auto [state, value] : { std::pair<DWORD, DWORD>{ 137, 0 }, { 136, 0 }, { 22, 1 }, { 7, 0 }, { 27, 1 }, { 19, 5 }, { 20, 6 } })
    {
        check(callMethod(device, d3d8::SetRenderState, state, value));
    }

    // Color/alpha ops and args, mag/min/mip filters, U/V addressing.
    for (const auto [state, value] : { std::pair<DWORD, DWORD>{ 1, 4 }, { 2, 2 }, { 3, 0 }, { 4, 4 }, { 5, 2 }, { 6, 0 }, { 16, 2 }, { 17, 2 }, { 18, 0 }, { 13, 3 }, { 14, 3 } })
    {
        check(callMethod(device, d3d8::SetTextureStageState, 0, state, value));
    }

    check(callMethod(device, d3d8::SetTextureStageState, 1, 1, 1)); // Stage 1 COLOROP = DISABLE
}

struct Difference
{
    size_t   painted   = 0;
    size_t   differing = 0;
    unsigned maxDelta  = 0;
};

auto compare(const std::vector<DWORD>& expected, const std::vector<DWORD>& actual) -> Difference
{
    auto result = Difference{};
    for (size_t i = 0; i < actual.size(); ++i)
    {
        if ((expected[i] & 0xffffff) != (kClearColor & 0xffffff))
        {
            ++result.painted;
        }

        for (const int shift : { 0, 8, 16 })
        {
            const auto delta = static_cast<unsigned>(std::abs(
                static_cast<int>((expected[i] >> shift) & 255) -
                static_cast<int>((actual[i] >> shift) & 255)));
            result.maxDelta  = (std::max)(delta, result.maxDelta);
            if (delta > kChannelTolerance)
            {
                ++result.differing;
                break;
            }
        }
    }

    return result;
}

void requireScaledQuadMatches(const bool textured)
{
    const auto d3d  = openDirect3D();
    auto       mode = d3d8::DisplayMode{};
    check(callMethod(d3d.Get(), d3d8::GetAdapterDisplayMode, 0, &mode));
    for (const float factor : kRenderScales)
    {
        const auto device = createDevice(d3d.Get(), mode.format, factor);
        DWORD      shader = 0;
        check(createScaleShader(device.Get(), &shader));
        ComPtr<IUnknown> buffer;
        // The scale adapter clears the FVF tag: a buffer tagged XYZRHW
        // bypasses shader input preparation in D3D8's software path.
        check(callMethod(device.Get(), d3d8::CreateVertexBuffer, 4 * sizeof(Vertex), kBufferUsage, 0, 0, buffer.GetAddressOf()));
        applyGameStates(device.Get());
        ComPtr<IUnknown> texture;
        if (textured)
        {
            texture = createCheckerTexture(device.Get());
        }

        check(callMethod(device.Get(), d3d8::SetTexture, 0, static_cast<void*>(texture.Get())));

        const auto expected   = render(device.Get(), buffer.Get(), shader, factor, true);
        const auto actual     = render(device.Get(), buffer.Get(), shader, factor, false);
        const auto difference = compare(expected, actual);
        CAPTURE(factor, textured, difference.painted, difference.differing, difference.maxDelta);
        REQUIRE(difference.painted >= actual.size() / 2);
        REQUIRE(difference.differing <= actual.size() / 10000);

        check(callMethod(device.Get(), d3d8::SetVertexShader, kGameVertexFormat));
        deleteScaleShader(device.Get(), shader);
    }
}

} // namespace

TEST_CASE("Scaled colored quads match independently scaled geometry", "[graphics-device]")
{
    requireScaledQuadMatches(false);
}

TEST_CASE("Scaled textured quads match independently scaled geometry", "[graphics-device]")
{
    requireScaledQuadMatches(true);
}
