#include "app/runtime.h"
#include "graphics/d3d8_abi.h"
#include "graphics/render_scale.h"
#include "textures/texture_replacements.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <ranges>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace tmloader;
using tmloader::callMethod;

namespace
{

constexpr UINT  kFrameWidth  = 640;
constexpr UINT  kFrameHeight = 480;
constexpr DWORD kBackground  = 0xff123456;
// The 72x96 card inside its padded 128x128 texture, as the game addresses it.
constexpr float kCardU = 72.0f / 128.0f;
constexpr float kCardV = 96.0f / 128.0f;

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

void check(const HRESULT result, const char* operation)
{
    INFO(operation);
    CAPTURE(result);
    REQUIRE(SUCCEEDED(result));
}

void state(void* device)
{
    // Lighting, clipping, cull, z, alpha test, dither, alpha blend, src/dest blend.
    for (const auto [setting, value] : { std::pair<DWORD, DWORD>{ 137, 0 }, { 136, 0 }, { 22, 1 }, { 7, 0 }, { 15, 0 }, { 26, 0 }, { 27, 1 }, { 19, 5 }, { 20, 6 } })
    {
        check(callMethod(device, d3d8::SetRenderState, setting, value), "Set render state");
    }

    // Select texture RGB/alpha and point sample so the test resolves each HD texel.
    for (const auto [setting, value] : { std::pair<DWORD, DWORD>{ 1, 2 }, { 2, 2 }, { 4, 2 }, { 5, 2 }, { 16, 1 }, { 17, 1 }, { 18, 0 }, { 13, 3 }, { 14, 3 } })
    {
        check(callMethod(device, d3d8::SetTextureStageState, 0U, setting, value), "Set texture state");
    }

    check(callMethod(device, d3d8::SetTextureStageState, 1U, 1UL, 1UL), "Disable second texture stage");
    check(callMethod(device, d3d8::SetVertexShader, kGameVertexFormat), "Set fixed-function vertex format");
}

struct OffscreenDevice
{
    ComPtr<IUnknown>        d3d;
    d3d8::PresentParameters parameters{};
    ComPtr<IUnknown>        device;
};

// Skips the calling test when there is no Direct3D 8 device or the desktop is not 32-bit.
auto createDevice() -> OffscreenDevice
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Once per process: the test executable may run several device cases in one go.
    static const bool lowered = lowerIntegrity();
    REQUIRE(lowered);
    const auto module  = LoadLibraryW(L"d3d8.dll");
    using Factory      = IUnknown*(WINAPI*)(UINT);
    const auto factory = [&]() -> Factory
    {
        if (!module)
        {
            return nullptr;
        }

        return reinterpret_cast<Factory>(GetProcAddress(module, "Direct3DCreate8"));
    }();
    if (!factory)
    {
        SKIP("Direct3D 8 unavailable");
    }

    OffscreenDevice result;
    result.d3d.Attach(factory(220));
    if (!result.d3d)
    {
        SKIP("Direct3D 8 unavailable");
    }

    auto mode = d3d8::DisplayMode{};
    check(callMethod(result.d3d.Get(), d3d8::GetAdapterDisplayMode, 0U, &mode), "Get display format");
    if (mode.format != d3d8::FormatA8R8G8B8 && mode.format != d3d8::FormatX8R8G8B8)
    {
        SKIP("test requires 32-bit desktop format");
    }

    auto& parameters  = result.parameters;
    parameters.width  = kFrameWidth;
    parameters.height = kFrameHeight;
    parameters.format = mode.format;
    parameters.count  = 1;
    parameters.swap   = d3d8::SwapEffectCopy;
    // No window is created or shown; no Present, focus or input call is used.
    parameters.window   = GetDesktopWindow();
    parameters.windowed = TRUE;
    parameters.flags    = 1;
    check(callMethod(result.d3d.Get(), d3d8::CreateDevice, 0U, d3d8::DeviceTypeHal, parameters.window, d3d8::kSoftwareVertexProcessing, &parameters, result.device.GetAddressOf()), "Create offscreen device");
    state(result.device.Get());
    return result;
}

auto sourcePixel(const UINT x, const UINT y, const bool highResolution) -> DWORD
{
    const auto scale = [&]() -> UINT
    {
        if (highResolution)
        {
            return 4;
        }

        return 1;
    }();
    if (x >= 72 * scale || y >= 96 * scale)
    {
        return 0x00ff00ff; // Transparent POT padding.
    }

    const auto alpha = [&]() -> DWORD
    {
        if (y < 8 * scale)
        {
            return 0;
        }

        if (x < 36 * scale)
        {
            return 128;
        }

        return 255;
    }();
    const auto color = [&]() -> DWORD
    {
        if (!highResolution)
        {
            return 0x507060;
        }

        if (x & 1)
        {
            return 0x2080d0;
        }

        return 0xd04020;
    }();
    return alpha << 24 | color;
}

auto texture(void* device, const bool highResolution) -> ComPtr<IUnknown>
{
    const auto extent = [&]() -> UINT
    {
        if (highResolution)
        {
            return 512;
        }

        return 128;
    }();
    ComPtr<IUnknown> result;
    check(callMethod(device, d3d8::CreateTexture, extent, extent, 1U, 0UL, d3d8::FormatA8R8G8B8, d3d8::PoolManaged, result.GetAddressOf()), "Create managed texture");
    auto locked = d3d8::LockedRect{};
    check(callMethod(result.Get(), d3d8::TextureLockRect, 0U, &locked, nullptr, 0UL), "Lock texture");
    for (UINT y = 0; y < extent; ++y)
    {
        for (UINT x = 0; x < extent; ++x)
        {
            reinterpret_cast<DWORD*>(locked.bits + y * locked.pitch)[x] = sourcePixel(x, y, highResolution);
        }
    }

    check(callMethod(result.Get(), d3d8::TextureUnlock, 0U), "Unlock texture");
    return result;
}

auto defaultPoolTexture(void* device) -> ComPtr<IUnknown>
{
    ComPtr<IUnknown> result;
    check(callMethod(device, d3d8::CreateTexture, 128U, 128U, 1U, 0UL, d3d8::FormatA8R8G8B8, 0UL, result.GetAddressOf()),
          "Create default-pool original");
    return result;
}

auto description(void* texture) -> d3d8::SurfaceDesc
{
    auto result = d3d8::SurfaceDesc{};
    check(callMethod(texture, d3d8::GetLevelDesc, 0U, &result), "Get texture level description");
    return result;
}

// The game sizes its quads from the original, so attaching must not change what it reports.
void checkLogicalSize(void* original)
{
    auto desc = d3d8::SurfaceDesc{};
    check(callMethod(original, d3d8::GetLevelDesc, 0U, &desc), "Get original level description");
    REQUIRE(desc.format == d3d8::FormatA8R8G8B8);
    REQUIRE(desc.width == 128);
    REQUIRE(desc.height == 128);
}

void checkBinding(void* device, void* expected)
{
    ComPtr<IUnknown> actual;
    check(callMethod(device, d3d8::GetTexture, 0U, actual.GetAddressOf()), "Get native texture binding");
    REQUIRE(static_cast<void*>(actual.Get()) == expected);
}

auto pixels(void* device, const UINT width = kFrameWidth, const UINT height = kFrameHeight) -> std::vector<DWORD>
{
    ComPtr<IUnknown> surface;
    check(callMethod(device, d3d8::GetBackBuffer, 0U, 0UL, surface.GetAddressOf()), "Get backbuffer");
    auto desc = d3d8::SurfaceDesc{};
    check(callMethod(surface.Get(), d3d8::SurfaceGetDesc, &desc), "Get backbuffer description");
    REQUIRE(desc.width == width);
    REQUIRE(desc.height == height);
    auto locked = d3d8::LockedRect{};
    check(callMethod(surface.Get(), d3d8::SurfaceLockRect, &locked, nullptr, d3d8::LockReadOnly), "Lock backbuffer");
    auto result = std::vector<DWORD>(width * height);
    for (UINT y = 0; y < height; ++y)
    {
        std::memcpy(result.data() + y * width, locked.bits + y * locked.pitch, width * 4);
    }

    check(callMethod(surface.Get(), d3d8::SurfaceUnlock), "Unlock backbuffer");
    return result;
}

void draw(void* device)
{
    const auto vertices = std::array<Vertex, 4>{ {
        { 31.5f, 31.5f, 0, 1, 0xffffffff, 0, 0, 0 },
        { 319.5f, 31.5f, 0, 1, 0xffffffff, 0, kCardU, 0 },
        { 31.5f, 415.5f, 0, 1, 0xffffffff, 0, 0, kCardV },
        { 319.5f, 415.5f, 0, 1, 0xffffffff, 0, kCardU, kCardV },
    } };
    check(callMethod(device, d3d8::DrawPrimitiveUP, d3d8::TriangleStrip, 2U, vertices.data(), static_cast<UINT>(sizeof(Vertex))), "Draw textured quad");
}

// Without useScope the bound texture is drawn as it is, which gives the tests an
// independent reference frame for both the original and the HD texture.
auto render(void* device, void* original, const bool useScope, const bool enabled, void* expectedDuringDraw, const bool fontsEnabled = true) -> std::vector<DWORD>
{
    check(callMethod(device, d3d8::SetTexture, 0U, original), "Bind original texture");
    check(callMethod(device, d3d8::Clear, 0UL, nullptr, 1UL, kBackground, 1.0f, 0UL), "Clear offscreen frame");
    check(callMethod(device, d3d8::BeginScene), "Begin scene");
    if (useScope)
    {
        const TextureDrawBinding binding(device, enabled, fontsEnabled);
        checkBinding(device, expectedDuringDraw);
        draw(device);
    }
    else
    {
        checkBinding(device, original);
        draw(device);
    }

    checkBinding(device, original);
    check(callMethod(device, d3d8::EndScene), "End scene");
    return pixels(device);
}

// Counts pixels whose colour differs; the backbuffer's alpha channel is not compared.
auto differences(const std::vector<DWORD>& first, const std::vector<DWORD>& second) -> size_t
{
    REQUIRE(first.size() == second.size());
    return static_cast<size_t>(std::ranges::count_if(std::views::zip(first, second), [](const auto& pair)
                                                     {
                                                         const auto [lhs, rhs] = pair;
                                                         return (lhs & 0xffffff) != (rhs & 0xffffff);
                                                     }));
}

void checkAlphaAndCrop(const std::vector<DWORD>& frame)
{
    const auto at = [&](const UINT x, const UINT y) -> unsigned long
    {
        return frame[y * kFrameWidth + x] & 0xffffff;
    };

    // Fully transparent source rows leave the background alone.
    REQUIRE(at(100, 40) == (kBackground & 0xffffff));
    // The power-of-two padding stays outside the drawn rectangle.
    REQUIRE(at(400, 200) == (kBackground & 0xffffff));
    // The cropped quad lands on the matching HD texel, one screen pixel per texel.
    REQUIRE(at(240, 200) == (sourcePixel(208, 168, true) & 0xffffff));
    REQUIRE(at(240, 200) != at(241, 200));

    // Half-transparent texels blend with the background, within rounding.
    const auto source = sourcePixel(20, 68, true);
    const auto actual = at(52, 100);
    for (const int shift : { 0, 8, 16 })
    {
        const auto expected = static_cast<int>(((((source >> shift) & 255) * 128) + (((kBackground >> shift) & 255) * 127) + 127) / 255);
        const auto observed = static_cast<int>((actual >> shift) & 255);
        REQUIRE(observed >= expected - 2);
        REQUIRE(observed <= expected + 2);
    }
}

// A real D3D8 resource owns this IUnknown through its private-data mechanism.
// The probe has no GPU calls and is never bound, so its final release is observable.
class LifetimeProbe final : public IUnknown
{
public:
    explicit LifetimeProbe(bool& destroyed)
    : destroyed_(destroyed)
    {
    }

    auto STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) -> HRESULT override
    {
        if (!output)
        {
            return E_POINTER;
        }

        *output = nullptr;
        if (iid != __uuidof(IUnknown))
        {
            return E_NOINTERFACE;
        }

        *output = this;
        AddRef();
        return S_OK;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override
    {
        return ++references_;
    }

    auto STDMETHODCALLTYPE Release() -> ULONG override
    {
        const auto left = --references_;
        if (!left)
        {
            destroyed_ = true;
            delete this;
        }

        return left;
    }

private:
    std::atomic<ULONG> references_{ 1 };
    bool&              destroyed_;
};

void checkOwnership(void* device, const bool defaultPool)
{
    for (const auto category : { TextureCategory::artwork, TextureCategory::font })
    {
        bool destroyed = false;
        auto original  = [&]() -> ComPtr<IUnknown>
        {
            if (defaultPool)
            {
                return defaultPoolTexture(device);
            }

            return texture(device, false);
        }();
        auto* const probe  = new LifetimeProbe(destroyed);
        const auto  result = attachTextureReplacement(original.Get(), probe, category);
        probe->Release();
        check(result, "Attach ownership probe");
        REQUIRE_FALSE(destroyed);
        original.Reset();
        REQUIRE(destroyed);
    }
}

auto fontTexture(void* device, const UINT scale) -> ComPtr<IUnknown>
{
    // Synthetic glyph masks exercise the native white-RGB/alpha-floor-1 atlas,
    // including soft coverage, 18px cells and high-frequency HD edge detail.
    ComPtr<IUnknown> result;
    check(callMethod(device, d3d8::CreateTexture, 256 * scale, 256 * scale, 1U, 0UL, d3d8::FormatA8R8G8B8, d3d8::PoolManaged, result.GetAddressOf()), "Create font atlas");
    auto locked = d3d8::LockedRect{};
    check(callMethod(result.Get(), d3d8::TextureLockRect, 0U, &locked, nullptr, 0UL), "Lock font atlas");
    for (UINT y = 0; y < 256 * scale; ++y)
    {
        for (UINT x = 0; x < 256 * scale; ++x)
        {
            const auto localX = x % (18 * scale);
            const auto localY = y % (18 * scale);
            const auto ink    = (localX >= 5 * scale && localX < 8 * scale && localY >= 4 * scale && localY < 15 * scale) ||
                                (localY >= 6 * scale && localY < 9 * scale && localX >= 5 * scale && localX < 15 * scale);
            const auto alpha  = [&]() -> DWORD
            {
                if (ink)
                {
                    return 144 + (x + y) % 112;
                }

                return 1;
            }();
            reinterpret_cast<DWORD*>(locked.bits + y * locked.pitch)[x] = alpha << 24 | 0xffffff;
        }
    }

    check(callMethod(result.Get(), d3d8::TextureUnlock, 0U), "Unlock font atlas");
    return result;
}

// A reference frame draws the bound atlas as it is, with the quads scaled on the
// CPU; otherwise the scale shader and the draw binding do the work.
auto renderFont(void* device, void* buffer, void* source, const DWORD shader, const float scale, const bool reference, const bool artworkEnabled = true, const bool fontsEnabled = true) -> std::vector<DWORD>
{
    check(callMethod(device, d3d8::SetTexture, 0U, source), "Bind font atlas");
    if (reference)
    {
        check(callMethod(device, d3d8::SetVertexShader, kGameVertexFormat), "Select font shader");
    }
    else
    {
        check(setScaleConstants(device, scale), "Set font scale");
        check(callMethod(device, d3d8::SetVertexShader, shader), "Select font shader");
    }

    check(callMethod(device, d3d8::Clear, 0UL, nullptr, 1UL, kBackground, 1.0f, 0UL), "Clear font frame");
    check(callMethod(device, d3d8::BeginScene), "Begin font scene");
    for (const bool shadow : { true, false })
    {
        for (int glyph = 0; glyph < 3; ++glyph)
        {
            const auto offset = static_cast<float>(shadow);
            const auto x      = 52.25f + glyph * 12.0f + offset;
            const auto y      = 48.5f + offset;
            // Native text uses a 16-unit quad sampling 17 atlas texels, offset by
            // (2,1) inside an 18px cell. Font metrics and UV denominators stay native.
            const auto u    = (2 + 18 * (glyph + 1)) / 256.0f;
            const auto v    = 19 / 256.0f;
            const auto tint = [&]() -> DWORD
            {
                if (shadow)
                {
                    return 0x90000000;
                }

                if (glyph == 1)
                {
                    return 0xb060d0ff;
                }

                return 0xffffd060;
            }();
            auto vertices = std::array<Vertex, 4>{ { { x, y, 0, 1, tint, 0, kCardU, kCardV }, { x + 16, y, 0, 1, tint, 0, u + 17 / 256.0f, v }, { x, y + 16, 0, 1, tint, 0, u, v + 17 / 256.0f }, { x + 16, y + 16, 0, 1, tint, 0, u + 17 / 256.0f, v + 17 / 256.0f } } };
            if (reference)
            {
                for (auto& vertex : vertices)
                {
                    vertex.x = (vertex.x + .5f) * scale - .5f;
                    vertex.y = (vertex.y + .5f) * scale - .5f;
                }
            }

            void* data{};
            check(callMethod(buffer, d3d8::BufferLock, 0U, static_cast<UINT>(sizeof(vertices)), &data, d3d8::LockDiscard), "Lock font vertices");
            std::memcpy(data, vertices.data(), sizeof(vertices));
            check(callMethod(buffer, d3d8::BufferUnlock), "Unlock font vertices");
            {
                const TextureDrawBinding replacement(device, !reference && artworkEnabled, !reference && fontsEnabled);
                check(callMethod(device, d3d8::DrawPrimitive, d3d8::TriangleStrip, 0U, 2U), "Draw font glyph");
            }

            checkBinding(device, source);
        }
    }

    check(callMethod(device, d3d8::EndScene), "End font scene");
    return pixels(device, static_cast<UINT>(640 * scale), static_cast<UINT>(480 * scale));
}

} // namespace

TEST_CASE("Draw binding swaps in the attached HD texture", "[graphics-device]")
{
    auto        gpu                 = createDevice();
    auto* const device              = gpu.device.Get();
    auto        original            = texture(device, false);
    const auto  originalDescription = description(original.Get());
    auto        replacement         = texture(device, true);
    const auto  originalFrame       = render(device, original.Get(), false, false, nullptr);
    const auto  hdFrame             = render(device, replacement.Get(), false, false, nullptr);
    check(attachTextureReplacement(original.Get(), replacement.Get()), "Attach HD texture");
    auto* const borrowedReplacement = replacement.Get();
    replacement.Reset(); // The original's private data must keep the real HD texture alive.

    SECTION("attaching leaves the original's description unchanged")
    {
        checkLogicalSize(original.Get());
        REQUIRE(description(original.Get()) == originalDescription);
    }

    SECTION("the HD texture is drawn through the original's UVs")
    {
        const auto enabled = render(device, original.Get(), true, true, borrowedReplacement);
        REQUIRE(differences(enabled, hdFrame) == 0);
        REQUIRE(differences(enabled, originalFrame) > 288U * 384U / 2);
        checkAlphaAndCrop(enabled);
        checkLogicalSize(original.Get());
        REQUIRE(description(original.Get()) == originalDescription);
    }

    SECTION("only the artwork switch selects an artwork replacement")
    {
        const auto artworkOff = render(device, original.Get(), true, false, original.Get());
        REQUIRE(differences(artworkOff, originalFrame) == 0);

        const auto fontsOff = render(device, original.Get(), true, true, borrowedReplacement, false);
        REQUIRE(differences(fontsOff, hdFrame) == 0);

        const auto bothOff = render(device, original.Get(), true, false, original.Get(), false);
        REQUIRE(differences(bothOff, originalFrame) == 0);
    }

    SECTION("a texture without a replacement is drawn as it is")
    {
        const auto missing = texture(device, false);
        REQUIRE(differences(render(device, missing.Get(), true, true, missing.Get()), originalFrame) == 0);
    }

    SECTION("nothing is bound when the stage is empty")
    {
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind texture");
        const TextureDrawBinding empty(device, true, true);
        checkBinding(device, nullptr);
    }

    SECTION("the managed replacement survives a device Reset")
    {
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind texture");
        check(callMethod(device, d3d8::Reset, &gpu.parameters), "Reset device with managed replacement");
        state(device);
        const auto afterReset = render(device, original.Get(), true, true, borrowedReplacement);
        REQUIRE(differences(afterReset, hdFrame) == 0);
        checkLogicalSize(original.Get());
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind before release");
        original.Reset();
    }
}

TEST_CASE("A managed original owns its replacement", "[graphics-device]")
{
    const auto gpu = createDevice();
    checkOwnership(gpu.device.Get(), false);
}

TEST_CASE("Default-pool originals can be released and recreated around a Reset", "[graphics-device]")
{
    auto        gpu     = createDevice();
    auto* const device  = gpu.device.Get();
    auto        hd      = texture(device, true);
    const auto  hdFrame = render(device, hd.Get(), false, false, nullptr);
    check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind reference texture");
    hd.Reset();

    // Native DEFAULT resources must be released before Reset, then recreated.
    // Repeat the complete attachment path so surviving managed test resources do
    // not conceal a leaked original or a stale replacement after reloading.
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        auto       original            = defaultPoolTexture(device);
        const auto originalDescription = description(original.Get());
        auto       replacement         = texture(device, true);
        check(attachTextureReplacement(original.Get(), replacement.Get()), "Attach to default-pool original");
        auto* const borrowedReplacement = replacement.Get();
        replacement.Reset();
        const auto actual = render(device, original.Get(), true, true, borrowedReplacement);
        REQUIRE(differences(actual, hdFrame) == 0);
        REQUIRE(description(original.Get()) == originalDescription);
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind default-pool original");
        original.Reset();
        check(callMethod(device, d3d8::Reset, &gpu.parameters), "Reset after default-pool original teardown");
        state(device);
    }

    // Reset fails while a default-pool resource is alive, so it also covers the probed originals.
    checkOwnership(device, true);
    check(callMethod(device, d3d8::Reset, &gpu.parameters), "Reset after default-pool ownership probe");
}

TEST_CASE("Font atlas replacement matches a directly drawn HD atlas", "[graphics-device]")
{
    auto        gpu         = createDevice();
    auto* const device      = gpu.device.Get();
    const auto  original    = fontTexture(device, 1);
    const auto  replacement = fontTexture(device, 4);
    check(attachTextureReplacement(original.Get(), replacement.Get(), TextureCategory::font), "Attach font replacement");
    const auto nativeDescription = description(original.Get());
    for (const float scale : { 3.0f, 4.5f })
    {
        CAPTURE(scale);
        gpu.parameters.width  = static_cast<UINT>(640 * scale);
        gpu.parameters.height = static_cast<UINT>(480 * scale);
        check(callMethod(device, d3d8::Reset, &gpu.parameters), "Reset for font resolution");
        state(device);
        // Match native text modulation/linear sampling, including per-vertex
        // opacity, colored glyphs and a separately drawn translucent shadow.
        for (const auto [setting, value] : { std::pair<DWORD, DWORD>{ 1, 4 }, { 3, 0 }, { 4, 4 }, { 6, 0 }, { 16, 2 }, { 17, 2 } })
        {
            check(callMethod(device, d3d8::SetTextureStageState, 0U, setting, value), "Set native font sampling");
        }

        ComPtr<IUnknown> buffer;
        check(callMethod(device, d3d8::CreateVertexBuffer, static_cast<UINT>(4 * sizeof(Vertex)), 0x238UL, 0UL, 0UL, buffer.GetAddressOf()), "Create font vertices");
        check(callMethod(device, d3d8::SetStreamSource, 0U, buffer.Get(), static_cast<UINT>(sizeof(Vertex))), "Bind font vertices");
        DWORD shader{};
        check(createScaleShader(device, &shader), "Create font scale shader");

        const auto expected = renderFont(device, buffer.Get(), replacement.Get(), shader, scale, true);
        const auto actual   = renderFont(device, buffer.Get(), original.Get(), shader, scale, false);
        REQUIRE(differences(expected, actual) == 0);

        // The fixtures have to tell the two atlases apart for the comparisons to mean anything.
        const auto native = renderFont(device, buffer.Get(), original.Get(), shader, scale, true);
        REQUIRE(differences(expected, native) > 100U);

        const auto artworkOff = renderFont(device, buffer.Get(), original.Get(), shader, scale, false, false, true);
        REQUIRE(differences(expected, artworkOff) == 0);

        const auto fontsOff = renderFont(device, buffer.Get(), original.Get(), shader, scale, false, true, false);
        REQUIRE(differences(native, fontsOff) == 0);

        const auto bothOff = renderFont(device, buffer.Get(), original.Get(), shader, scale, false, false, false);
        REQUIRE(differences(native, bothOff) == 0);

        const auto visible = std::ranges::count_if(actual, [](const DWORD pixel)
                                                   {
                                                       return (pixel & 0xffffff) != (kBackground & 0xffffff);
                                                   });
        REQUIRE(visible > 1000);
        REQUIRE(description(original.Get()) == nativeDescription);
        check(callMethod(device, d3d8::SetVertexShader, kGameVertexFormat), "Restore font vertex format");
        deleteScaleShader(device, shader);
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind font atlas");
        check(callMethod(device, d3d8::SetStreamSource, 0U, static_cast<void*>(nullptr), 0U), "Unbind font vertices");
    }
}
