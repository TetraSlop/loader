// The fixed-function key as ff.cpp writes it and the GLSL generators read it, and the uniform layout they share.
#pragma once

#include "gl.h"

#include <array>

namespace gl::ff
{

enum TextureKind : uint32_t
{
    kNoTexture, // the stage never reads its texture
    kWhite,     // the stage reads a texture but none is bound
    kTexture2D,
    kTextureCube,
};

enum MaterialSource : uint32_t
{
    kFromMaterial,
    kFromDiffuse,
    kFromSpecular,
};

// All uint32_t, so the struct copies into the key's words with no padding; fields that do not apply stay zero so equal
// pipelines get equal keys.
struct Stage
{
    uint32_t                colorOp;
    uint32_t                alphaOp;
    std::array<uint32_t, 3> colorArgs;
    std::array<uint32_t, 3> alphaArgs;
    uint32_t                resultTemp;
    uint32_t                texture;    // TextureKind
    uint32_t                coordIndex; // D3DTSS_TEXCOORDINDEX's set
    uint32_t                coordGen;   // D3DTSS_TEXCOORDINDEX's mode, shifted down
    uint32_t                transform;  // D3DTSS_TEXTURETRANSFORMFLAGS
};

struct Config
{
    uint32_t                fvf;
    uint32_t                lighting;
    uint32_t                normalize;
    uint32_t                localViewer;
    uint32_t                specular;
    std::array<uint32_t, 4> sources; // MaterialSource for diffuse, ambient, specular, emissive
    uint32_t                lightCount;
    std::array<uint32_t, 8> lights; // D3DLIGHTTYPE
    uint32_t                fog;
    uint32_t                tableFog; // D3DFOGMODE, applied per vertex
    uint32_t                vertexFog;
    uint32_t                rangeFog;
    uint32_t                stageCount;
    std::array<Stage, 8>    stages;
};

auto Decode(const FixedFunctionKey& key) -> Config;

// Bit n set: the D3DTEXTUREOP reads argument n (D3DTSS_COLORARGn or ALPHAARGn).
auto UsedArgs(uint32_t op) noexcept -> unsigned;

// The vertex stage's vec4 ff_params[]: these slots, then kLightParams per enabled light.
enum Param : int
{
    kViewport,  // X, Y, 2 / Width, 2 / Height
    kDepth,     // MinZ, 1 / (MaxZ - MinZ), table fog uses w (1) or z (0), material power
    kFogParams, // start, end, 1 / (end - start), density
    kAmbient,
    kMaterialDiffuse,
    kMaterialAmbient,
    kMaterialSpecular,
    kMaterialEmissive,
    kLightParamsStart,
};

enum LightParam : int
{
    kLightDiffuse,
    kLightSpecular,
    kLightAmbient,
    kLightPosition,    // camera space
    kLightDirection,   // camera space, normalized
    kLightAttenuation, // a0, a1, a2, range
    kLightSpot,        // cos(Theta / 2), cos(Phi / 2), 1 / (cos(Theta / 2) - cos(Phi / 2)), Falloff
    kLightParams,
};

} // namespace gl::ff
