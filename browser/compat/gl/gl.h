// WebGL2 for the Direct3D 8 device: each draw turns the device's D3D8 state into a GL program and GL state.
#pragma once

#include "../dx/d3d8_null.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gl
{

using nulldx::DeviceState;

// Locations are fixed so a program links once whatever buffers feed it.
enum Attribute : uint32_t
{
    kPosition     = 0, // xyz, or xyzrhw when pretransformed
    kBlendWeights = 1,
    kNormal       = 2,
    kPointSize    = 3,
    kDiffuse      = 4,
    kSpecular     = 5,
    kTexCoord0    = 6, // kTexCoord0 + n for n < 8
    kAttributes   = 14,
};

struct Element
{
    Attribute attribute;
    uint32_t  stream;
    uint32_t  offset;
    uint32_t  components;
    // A D3DCOLOR bound as its bytes lie, so the shader swizzles BGRA with .zyxw: WebGL2 has no BGRA vertex format.
    bool color;
};

// Offsets in stream 0.
auto FvfLayout(DWORD fvf) -> std::vector<Element>;
auto TexCoordSize(DWORD fvf, uint32_t set) noexcept -> uint32_t;

inline auto Rgba(D3DCOLOR argb) noexcept -> std::array<float, 4>
{
    return { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f };
}

// ARGB as D3D stores it to RGBA as GL does, or back.
inline auto SwapRedBlue(uint32_t color) noexcept -> uint32_t
{
    return (color & 0xFF00FF00) | ((color >> 16) & 0xFF) | ((color & 0xFF) << 16);
}

// The GLSL both generators write, which programs.cpp completes:
//
// - GLSL ES 3.00 with its own "#version 300 es" and precision lines, and no main(). A vertex stage defines
//   `vec4 VertexShader()`, which writes the varyings and returns oPos in D3D clip space (0 <= z <= w, y up). A fragment
//   stage defines `vec4 PixelShader()`, which returns the colour before fog and alpha test.
// - programs.cpp appends main(). The vertex main sets the varyings to their defaults (diffuse 1, specular 0, tex
//   (0,0,0,1), fog 1) and then calls VertexShader(), so a stage writes only what it has; it then shifts D3D8's pixel
//   centres onto GL's, flips y for the render target and maps z to GL's range. The fragment main calls PixelShader()
//   and applies fog (from v_fog and D3DRS_FOGCOLOR) and the alpha test, from render states.
// - Varyings, declared by both stages: `vec4 v_diffuse; vec4 v_specular; vec4 v_tex[8]; float v_fog;`. The vertex stage
//   saturates diffuse, specular and fog. v_tex[n] is what texture stage n samples with (after D3DTSS_TEXCOORDINDEX and
//   texture transforms, for fixed function).
// - Inputs: `layout(location = N) in vec4 ...;` at the Element locations.
// - Samplers: s0..s7 for stage n, declared only when used; sampler2D, or samplerCube where the key says the stage holds
//   a cube texture.
// - Constants: c[96] (vertex) and pc[8] (pixel), loaded from DeviceState by programs.cpp.
// - Names starting kr_ belong to programs.cpp's main().
//
// The key holds only what changes the generated code, so programs are cached by it. With a vertex shader, the draw uses
// only FixedFunctionFragment of KeyFor(state, 0).
using FixedFunctionKey = std::vector<uint32_t>;
auto KeyFor(const DeviceState& state, DWORD fvf) -> FixedFunctionKey;
auto FixedFunctionVertex(const FixedFunctionKey& key) -> std::string;
auto FixedFunctionFragment(const FixedFunctionKey& key) -> std::string;
void FixedFunctionUniforms(unsigned program, const DeviceState& state, DWORD fvf);

// function is the token stream D3DXAssembleShader fills (see d3dx8_shader.cpp for how the source text travels in it);
// nullptr for a declaration-only shader, which runs the fixed-function pipeline on the declared layout.
//
// Declaration registers map by D3DVSDE meaning onto Attribute: 0 position, 1 weights, 3 normal, 4 point size, 5
// diffuse, 6 specular, 7-14 tex0-7. Registers 2 and 15-16 (tweening) get no Element. A register with no meaning takes
// the first free location below kAttributes.
struct VertexProgram
{
    std::vector<Element> layout;
    std::string          glsl; // empty: fixed function
};

auto TranslateVertexShader(const DWORD* declaration, const DWORD* function) -> VertexProgram;
auto TranslatePixelShader(const DWORD* function) -> std::string;

// Without a context, as under Node, each does nothing and the device stays null.
void Start(UINT width, UINT height);
void Present(nulldx::NullDevice& device);
void Clear(
    nulldx::NullDevice& device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);

struct DrawCall
{
    D3DPRIMITIVETYPE type;
    UINT             primitives;
    UINT             start; // first vertex, or first index when indexed
    bool             indexed = false;
    // DrawPrimitiveUP: the caller's memory in place of stream 0.
    const void* vertices = nullptr;
    UINT        stride   = 0;
};

void Draw(nulldx::NullDevice& device, const DrawCall& call);

// Takes the copies Copy sent back whose pixels have arrived.
void BeginScene();

// False when either surface is in CPU memory, which the device copies.
auto Copy(nulldx::NullSurface* from, const RECT* rects, UINT count, nulldx::NullSurface* to, const POINT* points) -> bool;

} // namespace gl
