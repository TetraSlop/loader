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

constexpr UINT  frameWidth  = 640;
constexpr UINT  frameHeight = 480;
constexpr DWORD background  = 0xff123456;

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

void require(const bool condition, const char* message)
{
    INFO(message);
    REQUIRE(condition);
}

void check(const HRESULT result, const char* operation)
{
    INFO(operation);
    CAPTURE(result);
    REQUIRE(SUCCEEDED(result));
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
    check(callMethod(device, d3d8::CreateTexture, extent, extent, 1U, 0UL, d3d8::formatA8R8G8B8, d3d8::poolManaged, result.GetAddressOf()), "Create managed texture");
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
    check(callMethod(device, d3d8::CreateTexture, 128U, 128U, 1U, 0UL, d3d8::formatA8R8G8B8, 0UL, result.GetAddressOf()),
          "Create default-pool original");
    return result;
}

auto description(void* texture) -> d3d8::SurfaceDesc
{
    auto result = d3d8::SurfaceDesc{};
    check(callMethod(texture, d3d8::GetLevelDesc, 0U, &result), "Get texture level description");
    return result;
}

void checkLogicalSize(void* original)
{
    auto desc = d3d8::SurfaceDesc{};
    check(callMethod(original, d3d8::GetLevelDesc, 0U, &desc), "Get original level description");
    require(desc.format == d3d8::formatA8R8G8B8 && desc.width == 128 && desc.height == 128,
            "Replacement changed the original texture's format or logical dimensions");
}

void checkBinding(void* device, void* expected)
{
    ComPtr<IUnknown> actual;
    check(callMethod(device, d3d8::GetTexture, 0U, actual.GetAddressOf()), "Get native texture binding");
    require(actual.Get() == expected, "Unexpected native texture binding");
}

auto pixels(void* device, const UINT width = frameWidth, const UINT height = frameHeight) -> std::vector<DWORD>
{
    ComPtr<IUnknown> surface;
    check(callMethod(device, d3d8::GetBackBuffer, 0U, 0UL, surface.GetAddressOf()), "Get backbuffer");
    auto desc = d3d8::SurfaceDesc{};
    check(callMethod(surface.Get(), d3d8::SurfaceGetDesc, &desc), "Get backbuffer description");
    require(desc.width == width && desc.height == height, "Unexpected backbuffer dimensions");
    auto locked = d3d8::LockedRect{};
    check(callMethod(surface.Get(), d3d8::SurfaceLockRect, &locked, nullptr, d3d8::lockReadOnly), "Lock backbuffer");
    auto result = std::vector<DWORD>(width * height);
    for (UINT y = 0; y < height; ++y)
    {
        std::memcpy(result.data() + y * width, locked.bits + y * locked.pitch, width * 4);
    }

    check(callMethod(surface.Get(), d3d8::SurfaceUnlock), "Unlock backbuffer");
    return result;
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
    check(callMethod(device, d3d8::SetVertexShader, gameVertexFormat), "Set fixed-function vertex format");
}

void draw(void* device)
{
    // Use the game's original padded-texture UVs, even when drawing 4x larger.
    constexpr auto u        = 72.0f / 128.0f;
    constexpr auto v        = 96.0f / 128.0f;
    const auto     vertices = std::array<Vertex, 4>{ {
        { 31.5f, 31.5f, 0, 1, 0xffffffff, 0, 0, 0 },
        { 319.5f, 31.5f, 0, 1, 0xffffffff, 0, u, 0 },
        { 31.5f, 415.5f, 0, 1, 0xffffffff, 0, 0, v },
        { 319.5f, 415.5f, 0, 1, 0xffffffff, 0, u, v },
    } };
    check(callMethod(device, d3d8::DrawPrimitiveUP, d3d8::triangleStrip, 2U, vertices.data(), static_cast<UINT>(sizeof(Vertex))), "Draw textured quad");
}

auto render(void* device, void* original, const bool useScope, const bool enabled, void* expectedDuringDraw, const bool fontsEnabled = true) -> std::vector<DWORD>
{
    check(callMethod(device, d3d8::SetTexture, 0U, original), "Bind original texture");
    check(callMethod(device, d3d8::Clear, 0UL, nullptr, 1UL, background, 1.0f, 0UL), "Clear offscreen frame");
    check(callMethod(device, d3d8::BeginScene), "Begin scene");
    if (useScope)
    {
        const TextureDrawBinding binding(device, enabled, fontsEnabled);
        checkBinding(device, expectedDuringDraw);
        draw(device);
    }
    else
    {
        // Independent reference: bind and render the real HD texture directly.
        checkBinding(device, original);
        draw(device);
    }

    checkBinding(device, original);
    check(callMethod(device, d3d8::EndScene), "End scene");
    return pixels(device);
}

auto differences(const std::vector<DWORD>& first, const std::vector<DWORD>& second) -> size_t
{
    require(first.size() == second.size(), "Frame sizes differ");
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
        return frame[y * frameWidth + x] & 0xffffff;
    };
    require(at(100, 40) == (background & 0xffffff), "Transparent source pixels overwrite background");
    require(at(400, 200) == (background & 0xffffff), "Padded texture escapes the source rectangle");
    require(at(240, 200) == (sourcePixel(208, 168, true) & 0xffffff), "POT crop samples the wrong HD texel");
    require(at(240, 200) != at(241, 200), "One-pixel HD detail was lost");
    const auto source = sourcePixel(20, 68, true);
    const auto actual = at(52, 100);
    for (const int shift : { 0, 8, 16 })
    {
        const auto expected = static_cast<int>(((((source >> shift) & 255) * 128) + (((background >> shift) & 255) * 127) + 127) / 255);
        const auto observed = static_cast<int>((actual >> shift) & 255);
        require(observed >= expected - 2 && observed <= expected + 2, "Partial alpha coverage changed");
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
        require(!destroyed, "Original failed to retain replacement after caller Release");
        original.Reset();
        require(destroyed, "Original Release leaked its private replacement reference");
    }
}

void checkDefaultPoolLifecycle(void* device, d3d8::PresentParameters& parameters, const std::vector<DWORD>& expected)
{
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
        require(differences(actual, expected) == 0, "Reloaded default-pool original lost HD replacement");
        require(description(original.Get()) == originalDescription, "Default-pool original description changed");
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind default-pool original");
        original.Reset();
        check(callMethod(device, d3d8::Reset, &parameters), "Reset after default-pool original teardown");
        state(device);
    }

    checkOwnership(device, true);
    check(callMethod(device, d3d8::Reset, &parameters), "Reset after default-pool ownership probe");
}

auto fontTexture(void* device, const UINT scale) -> ComPtr<IUnknown>
{
    // Synthetic glyph masks exercise the native white-RGB/alpha-floor-1 atlas,
    // including soft coverage, 18px cells and high-frequency HD edge detail.
    ComPtr<IUnknown> result;
    check(callMethod(device, d3d8::CreateTexture, 256 * scale, 256 * scale, 1U, 0UL, d3d8::formatA8R8G8B8, d3d8::poolManaged, result.GetAddressOf()), "Create font atlas");
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

auto renderFont(void* device, void* buffer, void* source, const DWORD shader, const float scale, const bool reference, const bool artworkEnabled = true, const bool fontsEnabled = true) -> std::vector<DWORD>
{
    check(callMethod(device, d3d8::SetTexture, 0U, source), "Bind font atlas");
    if (reference)
    {
        check(callMethod(device, d3d8::SetVertexShader, gameVertexFormat), "Select font shader");
    }
    else
    {
        check(setScaleConstants(device, scale), "Set font scale");
        check(callMethod(device, d3d8::SetVertexShader, shader), "Select font shader");
    }

    check(callMethod(device, d3d8::Clear, 0UL, nullptr, 1UL, background, 1.0f, 0UL), "Clear font frame");
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
            auto vertices = std::array<Vertex, 4>{ { { x, y, 0, 1, tint, 0, u, v }, { x + 16, y, 0, 1, tint, 0, u + 17 / 256.0f, v }, { x, y + 16, 0, 1, tint, 0, u, v + 17 / 256.0f }, { x + 16, y + 16, 0, 1, tint, 0, u + 17 / 256.0f, v + 17 / 256.0f } } };
            if (reference)
            {
                for (auto& vertex : vertices)
                {
                    vertex.x = (vertex.x + .5f) * scale - .5f;
                    vertex.y = (vertex.y + .5f) * scale - .5f;
                }
            }

            void* data{};
            check(callMethod(buffer, d3d8::BufferLock, 0U, static_cast<UINT>(sizeof(vertices)), &data, d3d8::lockDiscard), "Lock font vertices");
            std::memcpy(data, vertices.data(), sizeof(vertices));
            check(callMethod(buffer, d3d8::BufferUnlock), "Unlock font vertices");
            {
                const TextureDrawBinding replacement(device, !reference && artworkEnabled, !reference && fontsEnabled);
                check(callMethod(device, d3d8::DrawPrimitive, d3d8::triangleStrip, 0U, 2U), "Draw font glyph");
            }

            checkBinding(device, source);
        }
    }

    check(callMethod(device, d3d8::EndScene), "End font scene");
    return pixels(device, static_cast<UINT>(640 * scale), static_cast<UINT>(480 * scale));
}

void checkFontRendering(void* device, d3d8::PresentParameters& parameters)
{
    auto original    = fontTexture(device, 1);
    auto replacement = fontTexture(device, 4);
    check(attachTextureReplacement(original.Get(), replacement.Get(), TextureCategory::font), "Attach font replacement");
    const auto nativeDescription = description(original.Get());
    for (const float scale : { 3.0f, 4.5f })
    {
        parameters.width  = static_cast<UINT>(640 * scale);
        parameters.height = static_cast<UINT>(480 * scale);
        check(callMethod(device, d3d8::Reset, &parameters), "Reset for font resolution");
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
        require(differences(expected, actual) == 0, "Font UV, tint, alpha or shadow differs from independent reference");
        const auto native = renderFont(device, buffer.Get(), original.Get(), shader, scale, true);
        require(differences(expected, native) > 100, "Font fixtures do not distinguish original and enhanced glyphs");
        require(differences(expected, renderFont(device, buffer.Get(), original.Get(), shader, scale, false, false, true)) == 0,
                "Disabling artwork also disabled enhanced fonts");
        require(differences(native, renderFont(device, buffer.Get(), original.Get(), shader, scale, false, true, false)) == 0,
                "Enhanced artwork prevented selecting original fonts");
        require(differences(native, renderFont(device, buffer.Get(), original.Get(), shader, scale, false, false, false)) == 0,
                "Disabling both categories did not retain original fonts");
        const auto visible = std::ranges::count_if(actual, [](const DWORD pixel)
                                                   {
                                                       return (pixel & 0xffffff) != (background & 0xffffff);
                                                   });
        require(visible > 1000, "Font comparison contains no visible glyphs");
        require(description(original.Get()) == nativeDescription, "Font atlas metadata changed");
        check(callMethod(device, d3d8::SetVertexShader, gameVertexFormat), "Restore font vertex format");
        deleteScaleShader(device, shader);
        check(callMethod(device, d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind font atlas");
        check(callMethod(device, d3d8::SetStreamSource, 0U, static_cast<void*>(nullptr), 0U), "Unbind font vertices");
        printf("PASS: %gx native font UVs, tint, alpha and shadow match directly bound HD atlas\n", scale);
    }
}

} // namespace

TEST_CASE("Replacement textures preserve native rendering and lifecycle", "[graphics-device]")
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    require(lowerIntegrity(), "Could not enter test low-integrity context");
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

    ComPtr<IUnknown> d3d;
    d3d.Attach(factory(220));
    if (!d3d)
    {
        SKIP("Direct3D 8 unavailable");
    }

    auto mode = d3d8::DisplayMode{};
    check(callMethod(d3d.Get(), d3d8::GetAdapterDisplayMode, 0U, &mode), "Get display format");
    if (mode.format != d3d8::formatA8R8G8B8 && mode.format != d3d8::formatX8R8G8B8)
    {
        SKIP("test requires 32-bit desktop format");
    }

    auto parameters   = d3d8::PresentParameters{};
    parameters.width  = frameWidth;
    parameters.height = frameHeight;
    parameters.format = mode.format;
    parameters.count  = 1;
    parameters.swap   = d3d8::swapEffectCopy;
    // No window is created or shown; no Present, focus or input call is used.
    parameters.window   = GetDesktopWindow();
    parameters.windowed = TRUE;
    parameters.flags    = 1;
    ComPtr<IUnknown> device;
    check(callMethod(d3d.Get(), d3d8::CreateDevice, 0U, d3d8::deviceTypeHal, parameters.window, d3d8::softwareVertexProcessing, &parameters, device.GetAddressOf()), "Create offscreen device");
    state(device.Get());
    auto       original            = texture(device.Get(), false);
    const auto originalDescription = description(original.Get());
    auto       replacement         = texture(device.Get(), true);
    const auto originalFrame       = render(device.Get(), original.Get(), false, false, nullptr);
    const auto expected            = render(device.Get(), replacement.Get(), false, false, nullptr);
    require(attachTextureReplacement(nullptr, replacement.Get()) == E_INVALIDARG, "Null original accepted");
    require(attachTextureReplacement(original.Get(), nullptr) == E_INVALIDARG, "Null replacement accepted");
    require(attachTextureReplacement(original.Get(), original.Get()) == E_INVALIDARG, "Self attachment accepted");
    require(attachTextureReplacement(original.Get(), replacement.Get(), static_cast<TextureCategory>(99)) == E_INVALIDARG,
            "Unknown replacement category accepted");
    check(attachTextureReplacement(original.Get(), replacement.Get()), "Attach HD texture");
    checkLogicalSize(original.Get());
    require(description(original.Get()) == originalDescription, "Original level description changed after attachment");
    auto* const borrowedReplacement = replacement.Get();
    replacement.Reset(); // The original's private data must keep the real HD texture alive.
    const auto enabled = render(device.Get(), original.Get(), true, true, borrowedReplacement);
    require(differences(enabled, expected) == 0, "Replacement rendering differs from directly bound HD texture");
    require(differences(enabled, originalFrame) > 288U * 384U / 2, "Replacement failed to change visible detail");
    checkAlphaAndCrop(enabled);
    const auto disabled = render(device.Get(), original.Get(), true, false, original.Get());
    require(differences(disabled, originalFrame) == 0, "Disabling replacements failed to restore original artwork");
    require(differences(render(device.Get(), original.Get(), true, true, borrowedReplacement, false), expected) == 0,
            "Disabling enhanced fonts also disabled HD artwork");
    require(differences(render(device.Get(), original.Get(), true, false, original.Get(), false), originalFrame) == 0,
            "Disabling both categories did not retain original artwork");
    checkLogicalSize(original.Get());
    require(description(original.Get()) == originalDescription, "Original level description changed after drawing");
    auto missing = texture(device.Get(), false);
    require(differences(render(device.Get(), missing.Get(), true, true, missing.Get()), originalFrame) == 0,
            "Missing replacement did not retain original artwork");
    check(callMethod(device.Get(), d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind texture");
    {
        const TextureDrawBinding empty(device.Get(), true, true);
        checkBinding(device.Get(), nullptr);
    }

    missing.Reset();
    check(callMethod(device.Get(), d3d8::Reset, &parameters), "Reset device with managed replacement");
    state(device.Get());
    const auto afterReset = render(device.Get(), original.Get(), true, true, borrowedReplacement);
    require(differences(afterReset, expected) == 0, "Managed replacement did not survive device Reset");
    checkLogicalSize(original.Get());
    check(callMethod(device.Get(), d3d8::SetTexture, 0U, static_cast<void*>(nullptr)), "Unbind before release");
    original.Reset();
    checkOwnership(device.Get(), false);
    checkDefaultPoolLifecycle(device.Get(), parameters, expected);
    checkFontRendering(device.Get(), parameters);
    puts("PASS: HD detail, alpha, POT crop, toggle, native binding, logical size, ownership, managed Reset and default-pool reload");
}
