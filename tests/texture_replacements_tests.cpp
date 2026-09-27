#include "runtime.h"
#include "d3d8_abi.h"
#include "render_scale.h"
#include "texture_replacements.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace tmloader;

namespace {
constexpr UINT frameWidth = 640, frameHeight = 480;
constexpr DWORD background = 0xff123456;
struct Vertex { float x, y, z, rhw; DWORD color, specular; float u, v; };
struct Locked { int pitch; BYTE* data; };
static_assert(sizeof(Vertex) == 32);

template<class... Args> HRESULT call(void* object, int slot, Args... args) {
    using Method = HRESULT(WINAPI*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(object, args...);
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        fprintf(stderr, "%s: %08lX\n", operation, result);
        throw std::runtime_error(operation);
    }
}

DWORD sourcePixel(UINT x, UINT y, bool highResolution) {
    const UINT scale = highResolution ? 4 : 1;
    if (x >= 72 * scale || y >= 96 * scale) return 0x00ff00ff; // Transparent POT padding.
    const DWORD alpha = y < 8 * scale ? 0 : x < 36 * scale ? 128 : 255;
    const DWORD color = highResolution ? ((x & 1) ? 0x2080d0 : 0xd04020) : 0x507060;
    return alpha << 24 | color;
}

ComPtr<IUnknown> texture(void* device, bool highResolution) {
    const UINT extent = highResolution ? 512 : 128;
    ComPtr<IUnknown> result;
    check(call(device, 20, extent, extent, 1U, 0UL, 21UL, 1UL, result.GetAddressOf()), "Create managed texture");
    Locked locked{};
    check(call(result.Get(), 16, 0U, &locked, nullptr, 0UL), "Lock texture");
    for (UINT y = 0; y < extent; ++y) for (UINT x = 0; x < extent; ++x)
        reinterpret_cast<DWORD*>(locked.data + y * locked.pitch)[x] = sourcePixel(x, y, highResolution);
    check(call(result.Get(), 17, 0U), "Unlock texture");
    return result;
}

ComPtr<IUnknown> defaultPoolTexture(void* device) {
    ComPtr<IUnknown> result;
    check(call(device, 20, 128U, 128U, 1U, 0UL, 21UL, 0UL, result.GetAddressOf()),
        "Create default-pool original");
    return result;
}

std::array<DWORD, 8> description(void* texture) {
    std::array<DWORD, 8> result{};
    check(call(texture, 14, 0U, result.data()), "Get texture level description");
    return result;
}

void checkLogicalSize(void* original) {
    DWORD description[8]{};
    check(call(original, 14, 0U, description), "Get original level description");
    require(description[0] == 21 && description[6] == 128 && description[7] == 128,
        "Replacement changed the original texture's format or logical dimensions");
}

void checkBinding(void* device, void* expected) {
    ComPtr<IUnknown> actual;
    check(call(device, 60, 0U, actual.GetAddressOf()), "Get native texture binding");
    require(actual.Get() == expected, "Unexpected native texture binding");
}

std::vector<DWORD> pixels(void* device, UINT width = frameWidth, UINT height = frameHeight) {
    ComPtr<IUnknown> surface;
    check(call(device, 16, 0U, 0UL, surface.GetAddressOf()), "Get backbuffer");
    DWORD description[8]{};
    check(call(surface.Get(), 8, description), "Get backbuffer description");
    require(description[6] == width && description[7] == height,
        "Unexpected backbuffer dimensions");
    Locked locked{};
    check(call(surface.Get(), 9, &locked, nullptr, 0x10UL), "Lock backbuffer");
    std::vector<DWORD> result(width * height);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(result.data() + y * width, locked.data + y * locked.pitch, width * 4);
    check(call(surface.Get(), 10), "Unlock backbuffer");
    return result;
}

void state(void* device) {
    for (auto setting : {std::pair<DWORD, DWORD>{137, 0}, {136, 0}, {22, 1}, {7, 0},
                        {15, 0}, {26, 0}, {27, 1}, {19, 5}, {20, 6}})
        check(call(device, 50, setting.first, setting.second), "Set render state");
    // Select texture RGB/alpha and point sample so the test resolves each HD texel.
    for (auto setting : {std::pair<DWORD, DWORD>{1, 2}, {2, 2}, {4, 2}, {5, 2},
                        {16, 1}, {17, 1}, {18, 0}, {13, 3}, {14, 3}})
        check(call(device, 63, 0U, setting.first, setting.second), "Set texture state");
    check(call(device, 63, 1U, 1UL, 1UL), "Disable second texture stage");
    check(call(device, 76, gameVertexFormat), "Set fixed-function vertex format");
}

void draw(void* device) {
    // Use the game's original padded-texture UVs, even when drawing 4x larger.
    constexpr float u = 72.0f / 128.0f, v = 96.0f / 128.0f;
    const std::array<Vertex, 4> vertices{{
        {31.5f, 31.5f, 0, 1, 0xffffffff, 0, 0, 0},
        {319.5f, 31.5f, 0, 1, 0xffffffff, 0, u, 0},
        {31.5f, 415.5f, 0, 1, 0xffffffff, 0, 0, v},
        {319.5f, 415.5f, 0, 1, 0xffffffff, 0, u, v},
    }};
    check(call(device, 72, 5UL, 2U, vertices.data(), static_cast<UINT>(sizeof(Vertex))), "Draw textured quad");
}

std::vector<DWORD> render(void* device, void* original, bool useScope, bool enabled, void* expectedDuringDraw) {
    check(call(device, 61, 0U, original), "Bind original texture");
    check(call(device, 36, 0UL, nullptr, 1UL, background, 1.0f, 0UL), "Clear offscreen frame");
    check(call(device, 34), "Begin scene");
    if (useScope) {
        TextureDrawBinding binding(device, enabled);
        checkBinding(device, expectedDuringDraw);
        draw(device);
    } else {
        // Independent reference: bind and render the real HD texture directly.
        checkBinding(device, original);
        draw(device);
    }
    checkBinding(device, original);
    check(call(device, 35), "End scene");
    return pixels(device);
}

size_t differences(const std::vector<DWORD>& first, const std::vector<DWORD>& second) {
    require(first.size() == second.size(), "Frame sizes differ");
    size_t count = 0;
    for (size_t i = 0; i < first.size(); ++i)
        if ((first[i] & 0xffffff) != (second[i] & 0xffffff)) ++count;
    return count;
}

void checkAlphaAndCrop(const std::vector<DWORD>& frame) {
    const auto at = [&](UINT x, UINT y) { return frame[y * frameWidth + x] & 0xffffff; };
    require(at(100, 40) == (background & 0xffffff), "Transparent source pixels overwrite background");
    require(at(400, 200) == (background & 0xffffff), "Padded texture escapes the source rectangle");
    require(at(240, 200) == (sourcePixel(208, 168, true) & 0xffffff), "POT crop samples the wrong HD texel");
    require(at(240, 200) != at(241, 200), "One-pixel HD detail was lost");
    const DWORD source = sourcePixel(20, 68, true), actual = at(52, 100);
    for (int shift : {0, 8, 16}) {
        const int expected = ((((source >> shift) & 255) * 128) + (((background >> shift) & 255) * 127) + 127) / 255;
        const int observed = (actual >> shift) & 255;
        require(observed >= expected - 2 && observed <= expected + 2, "Partial alpha coverage changed");
    }
}

// A real D3D8 resource owns this IUnknown through its private-data mechanism.
// The probe has no GPU calls and is never bound, so its final release is observable.
class LifetimeProbe final : public IUnknown {
public:
    explicit LifetimeProbe(bool& destroyed) : destroyed_(destroyed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *output = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (!left) { destroyed_ = true; delete this; }
        return left;
    }
private:
    std::atomic<ULONG> references_{1};
    bool& destroyed_;
};

void checkOwnership(void* device, bool defaultPool) {
    bool destroyed = false;
    auto original = defaultPool ? defaultPoolTexture(device) : texture(device, false);
    auto* probe = new LifetimeProbe(destroyed);
    const HRESULT result = attachTextureReplacement(original.Get(), probe);
    probe->Release();
    check(result, "Attach ownership probe");
    require(!destroyed, "Original failed to retain replacement after caller Release");
    original.Reset();
    require(destroyed, "Original Release leaked its private replacement reference");
}

void checkDefaultPoolLifecycle(void* device, Present8& parameters, const std::vector<DWORD>& expected) {
    // Native DEFAULT resources must be released before Reset, then recreated.
    // Repeat the complete attachment path so surviving managed test resources do
    // not conceal a leaked original or a stale replacement after reloading.
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto original = defaultPoolTexture(device);
        const auto originalDescription = description(original.Get());
        auto replacement = texture(device, true);
        check(attachTextureReplacement(original.Get(), replacement.Get()), "Attach to default-pool original");
        auto* borrowedReplacement = replacement.Get();
        replacement.Reset();
        const auto actual = render(device, original.Get(), true, true, borrowedReplacement);
        require(differences(actual, expected) == 0, "Reloaded default-pool original lost HD replacement");
        require(description(original.Get()) == originalDescription, "Default-pool original description changed");
        check(call(device, 61, 0U, static_cast<void*>(nullptr)), "Unbind default-pool original");
        original.Reset();
        check(call(device, 14, &parameters), "Reset after default-pool original teardown");
        state(device);
    }
    checkOwnership(device, true);
    check(call(device, 14, &parameters), "Reset after default-pool ownership probe");
}

ComPtr<IUnknown> fontTexture(void* device, UINT scale) {
    // Synthetic glyph masks exercise the native white-RGB/alpha-floor-1 atlas,
    // including soft coverage, 18px cells and high-frequency HD edge detail.
    ComPtr<IUnknown> result;
    check(call(device, 20, 256 * scale, 256 * scale, 1U, 0UL, 21UL, 1UL,
        result.GetAddressOf()), "Create font atlas");
    Locked locked{};
    check(call(result.Get(), 16, 0U, &locked, nullptr, 0UL), "Lock font atlas");
    for (UINT y = 0; y < 256 * scale; ++y) for (UINT x = 0; x < 256 * scale; ++x) {
        const UINT localX = x % (18 * scale), localY = y % (18 * scale);
        const bool ink = (localX >= 5 * scale && localX < 8 * scale && localY >= 4 * scale && localY < 15 * scale) ||
                         (localY >= 6 * scale && localY < 9 * scale && localX >= 5 * scale && localX < 15 * scale);
        const DWORD alpha = ink ? 144 + (x + y) % 112 : 1;
        reinterpret_cast<DWORD*>(locked.data + y * locked.pitch)[x] = alpha << 24 | 0xffffff;
    }
    check(call(result.Get(), 17, 0U), "Unlock font atlas");
    return result;
}

std::vector<DWORD> renderFont(void* device, void* buffer, void* source,
    DWORD shader, float scale, bool reference) {
    check(call(device, 61, 0U, source), "Bind font atlas");
    if (!reference) check(setScaleConstants(device, scale), "Set font scale");
    check(call(device, 76, reference ? gameVertexFormat : shader), "Select font shader");
    check(call(device, 36, 0UL, nullptr, 1UL, background, 1.0f, 0UL), "Clear font frame");
    check(call(device, 34), "Begin font scene");
    for (bool shadow : {true, false}) for (int glyph = 0; glyph < 3; ++glyph) {
        float x = 52.25f + glyph * 12.0f + (shadow ? 1 : 0), y = 48.5f + (shadow ? 1 : 0);
        // Native text uses a 16-unit quad sampling 17 atlas texels, offset by
        // (2,1) inside an 18px cell. Font metrics and UV denominators stay native.
        const float u = (2 + 18 * (glyph + 1)) / 256.0f, v = 19 / 256.0f;
        const DWORD tint = shadow ? 0x90000000 : glyph == 1 ? 0xb060d0ff : 0xffffd060;
        std::array<Vertex, 4> vertices{{
            {x, y, 0, 1, tint, 0, u, v}, {x + 16, y, 0, 1, tint, 0, u + 17 / 256.0f, v},
            {x, y + 16, 0, 1, tint, 0, u, v + 17 / 256.0f},
            {x + 16, y + 16, 0, 1, tint, 0, u + 17 / 256.0f, v + 17 / 256.0f}
        }};
        if (reference) for (auto& vertex : vertices) {
            vertex.x = (vertex.x + .5f) * scale - .5f;
            vertex.y = (vertex.y + .5f) * scale - .5f;
        }
        void* data{};
        check(call(buffer, 11, 0U, static_cast<UINT>(sizeof(vertices)), &data, 0x2000UL), "Lock font vertices");
        std::memcpy(data, vertices.data(), sizeof(vertices));
        check(call(buffer, 12), "Unlock font vertices");
        {
            TextureDrawBinding replacement(device, !reference);
            check(call(device, 70, 5UL, 0U, 2U), "Draw font glyph");
        }
        checkBinding(device, source);
    }
    check(call(device, 35), "End font scene");
    return pixels(device, static_cast<UINT>(640 * scale), static_cast<UINT>(480 * scale));
}

void checkFontRendering(void* device, Present8& parameters) {
    auto original = fontTexture(device, 1), replacement = fontTexture(device, 4);
    check(attachTextureReplacement(original.Get(), replacement.Get()), "Attach font replacement");
    const auto nativeDescription = description(original.Get());
    for (float scale : {3.0f, 4.5f}) {
        parameters.width = static_cast<UINT>(640 * scale);
        parameters.height = static_cast<UINT>(480 * scale);
        check(call(device, 14, &parameters), "Reset for font resolution");
        state(device);
        // Match native text modulation/linear sampling, including per-vertex
        // opacity, colored glyphs and a separately drawn translucent shadow.
        for (auto setting : {std::pair<DWORD, DWORD>{1, 4}, {3, 0}, {4, 4}, {6, 0}, {16, 2}, {17, 2}})
            check(call(device, 63, 0U, setting.first, setting.second), "Set native font sampling");
        ComPtr<IUnknown> buffer;
        check(call(device, 23, static_cast<UINT>(4 * sizeof(Vertex)), 0x238UL, 0UL, 0UL,
            buffer.GetAddressOf()), "Create font vertices");
        check(call(device, 83, 0U, buffer.Get(), static_cast<UINT>(sizeof(Vertex))), "Bind font vertices");
        DWORD shader{};
        check(createScaleShader(device, &shader), "Create font scale shader");
        const auto expected = renderFont(device, buffer.Get(), replacement.Get(), shader, scale, true);
        const auto actual = renderFont(device, buffer.Get(), original.Get(), shader, scale, false);
        require(differences(expected, actual) == 0, "Font UV, tint, alpha or shadow differs from independent reference");
        size_t visible = 0;
        for (DWORD pixel : actual) if ((pixel & 0xffffff) != (background & 0xffffff)) ++visible;
        require(visible > 1000, "Font comparison contains no visible glyphs");
        require(description(original.Get()) == nativeDescription, "Font atlas metadata changed");
        check(call(device, 76, gameVertexFormat), "Restore font vertex format");
        deleteScaleShader(device, shader);
        check(call(device, 61, 0U, static_cast<void*>(nullptr)), "Unbind font atlas");
        check(call(device, 83, 0U, static_cast<void*>(nullptr), 0U), "Unbind font vertices");
        printf("PASS: %gx native font UVs, tint, alpha and shadow match directly bound HD atlas\n", scale);
    }
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(lowerIntegrity(), "Could not enter test low-integrity context");
        const auto module = LoadLibraryW(L"d3d8.dll");
        using Factory = IUnknown*(WINAPI*)(UINT);
        const auto factory = module ? reinterpret_cast<Factory>(GetProcAddress(module, "Direct3DCreate8")) : nullptr;
        if (!factory) { puts("SKIP: Direct3D 8 unavailable"); return 77; }
        ComPtr<IUnknown> d3d;
        d3d.Attach(factory(220));
        if (!d3d) { puts("SKIP: Direct3D 8 unavailable"); return 77; }
        DWORD mode[4]{};
        check(call(d3d.Get(), 8, 0U, mode), "Get display format");
        if (mode[3] != 21 && mode[3] != 22) { puts("SKIP: test requires 32-bit desktop format"); return 77; }
        Present8 parameters{};
        parameters.width = frameWidth; parameters.height = frameHeight;
        parameters.format = mode[3]; parameters.count = 1; parameters.swap = 3;
        // No window is created or shown; no Present, focus or input call is used.
        parameters.window = GetDesktopWindow(); parameters.windowed = TRUE; parameters.flags = 1;
        ComPtr<IUnknown> device;
        check(call(d3d.Get(), 15, 0U, 1UL, parameters.window, 0x20UL, &parameters, device.GetAddressOf()), "Create offscreen device");
        state(device.Get());
        auto original = texture(device.Get(), false);
        const auto originalDescription = description(original.Get());
        auto replacement = texture(device.Get(), true);
        const auto originalFrame = render(device.Get(), original.Get(), false, false, nullptr);
        const auto expected = render(device.Get(), replacement.Get(), false, false, nullptr);
        require(attachTextureReplacement(nullptr, replacement.Get()) == E_INVALIDARG, "Null original accepted");
        require(attachTextureReplacement(original.Get(), nullptr) == E_INVALIDARG, "Null replacement accepted");
        require(attachTextureReplacement(original.Get(), original.Get()) == E_INVALIDARG, "Self attachment accepted");
        check(attachTextureReplacement(original.Get(), replacement.Get()), "Attach HD texture");
        checkLogicalSize(original.Get());
        require(description(original.Get()) == originalDescription, "Original level description changed after attachment");
        auto* borrowedReplacement = replacement.Get();
        replacement.Reset(); // The original's private data must keep the real HD texture alive.
        const auto enabled = render(device.Get(), original.Get(), true, true, borrowedReplacement);
        require(differences(enabled, expected) == 0, "Replacement rendering differs from directly bound HD texture");
        require(differences(enabled, originalFrame) > 288U * 384U / 2, "Replacement failed to change visible detail");
        checkAlphaAndCrop(enabled);
        const auto disabled = render(device.Get(), original.Get(), true, false, original.Get());
        require(differences(disabled, originalFrame) == 0, "Disabling replacements failed to restore original artwork");
        checkLogicalSize(original.Get());
        require(description(original.Get()) == originalDescription, "Original level description changed after drawing");
        auto missing = texture(device.Get(), false);
        require(differences(render(device.Get(), missing.Get(), true, true, missing.Get()), originalFrame) == 0,
            "Missing replacement did not retain original artwork");
        check(call(device.Get(), 61, 0U, static_cast<void*>(nullptr)), "Unbind texture");
        {
            TextureDrawBinding empty(device.Get(), true);
            checkBinding(device.Get(), nullptr);
        }
        missing.Reset();
        check(call(device.Get(), 14, &parameters), "Reset device with managed replacement");
        state(device.Get());
        const auto afterReset = render(device.Get(), original.Get(), true, true, borrowedReplacement);
        require(differences(afterReset, expected) == 0, "Managed replacement did not survive device Reset");
        checkLogicalSize(original.Get());
        check(call(device.Get(), 61, 0U, static_cast<void*>(nullptr)), "Unbind before release");
        original.Reset();
        checkOwnership(device.Get(), false);
        checkDefaultPoolLifecycle(device.Get(), parameters, expected);
        checkFontRendering(device.Get(), parameters);
        puts("PASS: HD detail, alpha, POT crop, toggle, native binding, logical size, ownership, managed Reset and default-pool reload");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
