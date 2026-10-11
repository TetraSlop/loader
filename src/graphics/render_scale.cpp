#include "graphics/render_scale.h"
#include "graphics/d3d8_abi.h"

namespace tmloader
{
namespace
{

// Stream 0: float4 position, packed diffuse and specular colors, float2 UV.
constexpr DWORD kDeclaration[] = {
    0x20000000, 0x40030000, 0x40040001, 0x40040002, 0x40010003, 0xffffffff
};
// Hand-assembled vs.1.1: converts screen coordinates to clip space and keeps
// reciprocal W for perspective-correct interpolation. Other inputs pass through.
constexpr DWORD kProgram[] = {
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
    return callMethod(device, d3d8::CreateVertexShader, kDeclaration, kProgram, shader, DWORD{ 0 });
}

auto setScaleConstants(void* device, float factor) -> HRESULT
{
    // Maps pixel edges as (x + 0.5) * factor - 0.5, which is exact at 1x.
    const float offset       = 0.5f - 0.5f / factor;
    const float constants[8] = {
        2.0f / 640, -2.0f / 480, 1, 1, -1 + offset * 2 / 640, 1 - offset * 2 / 480, 0, 0
    };
    return callMethod(device, d3d8::SetVertexShaderConstant, DWORD{ 0 }, static_cast<const void*>(constants), DWORD{ 2 });
}

void deleteScaleShader(void* device, DWORD& shader)
{
    if (!shader)
    {
        return;
    }

    callMethod(device, d3d8::DeleteVertexShader, shader);
    shader = 0;
}

} // namespace tmloader
