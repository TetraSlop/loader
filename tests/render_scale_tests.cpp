#include <catch2/catch_test_macros.hpp>
#include "runtime.h"
#include "d3d8_abi.h"
#include "render_scale.h"
#include "window_settings.h"
#include <unknwn.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace tmloader;
template<class... Args>
HRESULT call(void* object, int slot, Args... args) {
    using Method = HRESULT(WINAPI*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(object, args...);
}
void check(HRESULT hr) {
    INFO("D3D8 HRESULT: " << hr);
    REQUIRE(SUCCEEDED(hr));
}
struct Vertex { float x, y, z, rhw; DWORD color, specular; float u, v; };
static_assert(sizeof(Vertex) == 32);
struct Locked { int pitch; BYTE* data; };

std::vector<DWORD> pixels(void* device, float factor) {
    ComPtr<IUnknown> surface;
    check(call(device, 16, 0, 0, surface.GetAddressOf()));
    DWORD desc[8]{};
    check(call(surface.Get(), 8, desc));
    REQUIRE(desc[6] == 640U * factor);
    REQUIRE(desc[7] == 480U * factor);
    Locked locked{};
    check(call(surface.Get(), 9, &locked, nullptr, 0x10));
    std::vector<DWORD> result(desc[6] * desc[7]);
    for (DWORD y = 0; y < desc[7]; ++y)
        std::memcpy(result.data() + y * desc[6], locked.data + y * locked.pitch, desc[6] * 4);
    check(call(surface.Get(), 10));
    return result;
}

std::vector<DWORD> render(void* device, void* buffer, DWORD shader, float factor, bool reference) {
    // A clipped quad with unequal W, interpolated alpha/color and noninteger
    // positions. CPU-scaled fixed-function rendering is the independent oracle.
    std::array<Vertex, 4> vertices{{
        {-12.5f, 15.75f, 0.25f, 0.5f, 0xffcc88aa, 0, 0, 0},
        {602.5f, 45.25f, 0.5f, 1.0f, 0xc088aaff, 0, 1, 0},
        {-4.5f, 470.75f, 0.25f, 0.75f, 0x8888ffaa, 0, 0, 1},
        {630.5f, 491.25f, 0.75f, 0.25f, 0xffffaacc, 0, 1, 1}
    }};
    if (reference) for (auto& v : vertices) {
        v.x = (v.x + 0.5f) * factor - 0.5f;
        v.y = (v.y + 0.5f) * factor - 0.5f;
    }
    void* data = nullptr;
    check(call(buffer, 11, 0, sizeof(vertices), &data, 0x2000));
    std::memcpy(data, vertices.data(), sizeof(vertices));
    check(call(buffer, 12));
    check(call(device, 83, 0, buffer, sizeof(Vertex)));
    if (!reference) check(setScaleConstants(device, factor));
    check(call(device, 76, reference ? gameVertexFormat : shader));
    check(call(device, 36, 0, nullptr, 1, 0xff123456, 1.0f, 0));
    check(call(device, 34));
    check(call(device, 70, 5, 0, 2));
    check(call(device, 35));
    return pixels(device, factor);
}

TEST_CASE("Scaled rendering matches independently scaled geometry", "[graphics-device]") {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    REQUIRE(tmloader::lowerIntegrity());
    // No window is created and no Present, focus or input API is called.
    auto module = LoadLibraryW(L"d3d8.dll");
    using Factory = IUnknown*(WINAPI*)(UINT);
    auto factory = reinterpret_cast<Factory>(GetProcAddress(module, "Direct3DCreate8"));
    if (!factory) { SKIP("Direct3D 8 unavailable"); }
    ComPtr<IUnknown> d3d;
    d3d.Attach(factory(220));
    if (!d3d) { SKIP("Direct3D 8 unavailable"); }
    DWORD mode[4]{};
    check(call(d3d.Get(), 8, 0, mode));
    for (float factor : renderScales) {
        Present8 pp{};
        pp.width = static_cast<UINT>(640 * factor); pp.height = static_cast<UINT>(480 * factor);
        pp.format = mode[3]; pp.count = 1; pp.swap = 3;
        pp.window = GetDesktopWindow(); pp.windowed = TRUE; pp.flags = 1;
        ComPtr<IUnknown> device;
        check(call(d3d.Get(), 15, 0, 1, pp.window, 0x20, &pp, device.GetAddressOf()));
        DWORD shader = 0;
        check(createScaleShader(device.Get(), &shader));
        ComPtr<IUnknown> buffer, texture;
        // The scale adapter clears the FVF tag: a buffer tagged XYZRHW
        // bypasses shader input preparation in D3D8's software path.
        check(call(device.Get(), 23, 4 * sizeof(Vertex), 0x238, 0, 0, buffer.GetAddressOf()));
        check(call(device.Get(), 20, 8, 8, 1, 0, 21, 1, texture.GetAddressOf()));
        Locked locked{};
        check(call(texture.Get(), 16, 0, &locked, nullptr, 0));
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
            reinterpret_cast<DWORD*>(locked.data + y * locked.pitch)[x] =
                ((x + y) % 2) ? 0xffdddddd : 0xff336699;
        check(call(texture.Get(), 17, 0));
        for (auto state : {std::pair<DWORD, DWORD>{137, 0}, {136, 0}, {22, 1}, {7, 0}, {27, 1}, {19, 5}, {20, 6}})
            check(call(device.Get(), 50, state.first, state.second));
        for (auto state : {std::pair<DWORD, DWORD>{1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0},
                           {16, 2}, {17, 2}, {18, 0}, {13, 3}, {14, 3}})
            check(call(device.Get(), 63, 0, state.first, state.second));
        check(call(device.Get(), 63, 1, 1, 1));
        for (bool textured : {false, true}) {
            check(call(device.Get(), 61, 0, textured ? texture.Get() : nullptr));
            auto expected = render(device.Get(), buffer.Get(), shader, factor, true);
            auto actual = render(device.Get(), buffer.Get(), shader, factor, false);
            size_t differing = 0, painted = 0;
            unsigned maxDelta = 0;
            for (size_t i = 0; i < actual.size(); ++i) {
                if ((expected[i] & 0xffffff) != 0x123456) ++painted;
                for (int shift : {0, 8, 16}) {
                    unsigned delta = static_cast<unsigned>(std::abs(
                        static_cast<int>((expected[i] >> shift) & 255) -
                        static_cast<int>((actual[i] >> shift) & 255)));
                    maxDelta = (std::max)(delta, maxDelta);
                    if (delta > 2) { ++differing; break; }
                }
            }
            printf("%gx %s: %zu pixels, %zu differ by >2, max delta %u\n",
                factor, textured ? "textured" : "colored", painted, differing, maxDelta);
            CAPTURE(factor, textured, painted, differing, maxDelta);
            REQUIRE(painted >= actual.size() / 2);
            REQUIRE(differing <= actual.size() / 10000);
        }
        check(call(device.Get(), 76, gameVertexFormat));
        deleteScaleShader(device.Get(), shader);
    }
    puts("PASS: actual rendering through 4K and 6x matches independently scaled geometry");
}
