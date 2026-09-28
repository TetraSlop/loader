#include "graphics/render_scale.h"

namespace tmloader
{
namespace
{

// D3D8 declarations live outside the shader: stream 0, float4 position,
// packed diffuse/specular colors, float2 UV. No change to the game's buffers.
constexpr DWORD declaration[] = {
    0x20000000, 0x40030000, 0x40040001, 0x40040002, 0x40010003, 0xffffffff
};
// Assembled vs.1.1, with each instruction retained below for review. Screen
// coordinates become clip coordinates; reciprocal W preserves perspective
// interpolation. Colors, UVs and depth retain the game's values.
constexpr DWORD program[] = {
    0xfffe0101,
    0x00000005,
    0x80030000,
    0x90e40000,
    0xa0e40000, // mul r0.xy, v0, c0
    0x00000002,
    0x80030000,
    0x80e40000,
    0xa0e40001, // add r0.xy, r0, c1
    0x00000001,
    0x80040000,
    0x90aa0000, // mov r0.z, v0.z
    0x00000001,
    0x80080000,
    0xa0ff0000, // mov r0.w, c0.w
    0x00000006,
    0x80080001,
    0x90ff0000, // rcp r1.w, v0.w
    0x00000005,
    0xc00f0000,
    0x80e40000,
    0x80ff0001, // mul oPos, r0, r1.w
    0x00000001,
    0xd00f0000,
    0x90e40001, // mov oD0, v1
    0x00000001,
    0xd00f0001,
    0x90e40002, // mov oD1, v2
    0x00000001,
    0xe0030000,
    0x90e40003, // mov oT0.xy, v3
    0x0000ffff
};

} // namespace

auto createScaleShader(void* device, DWORD* shader) -> HRESULT
{
    using Create = HRESULT(WINAPI*)(void*, const DWORD*, const DWORD*, DWORD*, DWORD);
    auto table   = *static_cast<void***>(device);
    return reinterpret_cast<Create>(table[75])(device, declaration, program, shader, 0);
}

auto setScaleConstants(void* device, float factor) -> HRESULT
{
    // Map pixel edges: (x + 0.5) * factor - 0.5. This also preserves the
    // original rasterization exactly at 1x, including half-pixel aligned quads.
    const float offset       = 0.5f - 0.5f / factor;
    const float constants[8] = {
        2.0f / 640, -2.0f / 480, 1, 1, -1 + offset * 2 / 640, 1 - offset * 2 / 480, 0, 0
    };
    auto table      = *static_cast<void***>(device);
    using Constants = HRESULT(WINAPI*)(void*, DWORD, const void*, DWORD);
    return reinterpret_cast<Constants>(table[79])(device, 0, constants, 2);
}

void deleteScaleShader(void* device, DWORD& shader)
{
    if (!shader)
        return;
    using Delete = HRESULT(WINAPI*)(void*, DWORD);
    auto table   = *static_cast<void***>(device);
    reinterpret_cast<Delete>(table[78])(device, shader);
    shader = 0;
}

} // namespace tmloader
