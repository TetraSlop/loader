// The Direct3D 8 fixed-function pipeline's key and uniforms; ff_vertex.cpp and ff_fragment.cpp write its GLSL. The
// semantics follow Microsoft's D3D8 documentation and the behaviour of Wine's wined3d fixed-function generator.
#include "ff.h"
#include "internal.h"

#include <d3dx8.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gl
{

namespace
{

using namespace ff;

auto FloatState(const DeviceState& state, D3DRENDERSTATETYPE type) noexcept -> float
{
    auto value = 0.0f;
    std::memcpy(&value, &state.renderStates[type], sizeof(value));
    return value;
}

auto Pretransformed(DWORD fvf) noexcept -> bool
{
    return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

// D3D8 lights only the first eight enabled lights.
auto EnabledLights(const DeviceState& state) -> std::vector<const D3DLIGHT8*>
{
    auto lights = std::vector<const D3DLIGHT8*>{};
    for (const auto& [index, light] : state.lights)
    {
        if (light.enabled && lights.size() < 8)
        {
            lights.push_back(&light.light);
        }
    }

    return lights;
}

auto Source(DWORD source, DWORD fvf, bool colorVertex) noexcept -> MaterialSource
{
    if (colorVertex && source == D3DMCS_COLOR1 && (fvf & D3DFVF_DIFFUSE))
    {
        return kFromDiffuse;
    }

    if (colorVertex && source == D3DMCS_COLOR2 && (fvf & D3DFVF_SPECULAR))
    {
        return kFromSpecular;
    }

    return kFromMaterial;
}

auto ReadsTexture(DWORD op, const std::array<uint32_t, 3>& args) noexcept -> bool
{
    if (op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM)
    {
        return true;
    }

    const auto used = UsedArgs(op);
    for (unsigned n = 0; n < 3; ++n)
    {
        if ((used & (1u << n)) && (args[n] & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
        {
            return true;
        }
    }

    return false;
}

auto KindOf(IDirect3DBaseTexture8* texture) -> TextureKind
{
    if (texture == nullptr)
    {
        return kWhite;
    }

    switch (texture->GetType())
    {
        case D3DRTYPE_TEXTURE:
            return kTexture2D;
        case D3DRTYPE_CUBETEXTURE:
            return kTextureCube;
        default:
            // The client makes no volume textures.
            return kWhite;
    }
}

void RouteCoordinates(Stage& stage, const std::array<DWORD, nulldx::kTextureStageStates>& ts) noexcept
{
    if (stage.texture != kTexture2D && stage.texture != kTextureCube)
    {
        return;
    }

    stage.coordIndex = ts[D3DTSS_TEXCOORDINDEX] & 7;
    stage.coordGen   = ts[D3DTSS_TEXCOORDINDEX] >> 16;
    stage.transform  = ts[D3DTSS_TEXTURETRANSFORMFLAGS] & (0xFF | D3DTTFF_PROJECTED);
}

// The inverse transpose of the world-view 3x3, as D3D8 transforms normals. GL reads this D3DMATRIX layout transposed,
// so it holds the inverse, which meets a column vector as D3D's row vector meets the inverse transpose.
auto NormalMatrix(const D3DMATRIX& m) noexcept -> std::array<float, 9>
{
    const auto at = [&](int r, int c)
    {
        return m.m[r % 3][c % 3];
    };

    auto cofactors = std::array<float, 9>{};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            cofactors[r * 3 + c] = at(r + 1, c + 1) * at(r + 2, c + 2) - at(r + 1, c + 2) * at(r + 2, c + 1);
        }
    }

    const auto det   = m.m[0][0] * cofactors[0] + m.m[0][1] * cofactors[1] + m.m[0][2] * cofactors[2];
    const auto scale = [&]() -> float
    {
        if (det != 0.0f)
        {
            return 1.0f / det;
        }

        return 0.0f;
    }();

    for (auto& value : cofactors)
    {
        value *= scale;
    }

    return cofactors;
}

auto TransformPoint(const D3DVECTOR& v, const D3DMATRIX& m, float w) noexcept -> std::array<float, 4>
{
    auto out = std::array<float, 4>{};
    for (int c = 0; c < 4; ++c)
    {
        out[c] = v.x * m.m[0][c] + v.y * m.m[1][c] + v.z * m.m[2][c] + w * m.m[3][c];
    }

    return out;
}

struct Locations
{
    Uniform worldView;
    Uniform projection;
    Uniform normalMatrix;
    Uniform params;
    Uniform textureMatrices;
    Uniform textureFactor;
};

// Keyed by program name, which is safe only because programs.cpp keeps its programs for the context's life.
auto LocationsFor(GLuint program) -> Locations&
{
    static auto cache = std::unordered_map<GLuint, Locations>{};
    if (const auto found = cache.find(program); found != cache.end())
    {
        return found->second;
    }

    auto& locations                    = cache[program];
    locations.worldView.location       = glGetUniformLocation(program, "ff_worldView");
    locations.projection.location      = glGetUniformLocation(program, "ff_projection");
    locations.normalMatrix.location    = glGetUniformLocation(program, "ff_normalMatrix");
    locations.params.location          = glGetUniformLocation(program, "ff_params");
    locations.textureMatrices.location = glGetUniformLocation(program, "ff_texture");
    locations.textureFactor.location   = glGetUniformLocation(program, "ff_textureFactor");
    return locations;
}

// 1e30 stands in for the infinity an empty range divides into.
auto InverseSpan(float from, float to) noexcept -> float
{
    if (to != from)
    {
        return 1.0f / (to - from);
    }

    return 1e30f;
}

} // namespace

namespace ff
{

auto UsedArgs(uint32_t op) noexcept -> unsigned
{
    switch (op)
    {
        case D3DTOP_DISABLE:
        case D3DTOP_BUMPENVMAP:
        case D3DTOP_BUMPENVMAPLUMINANCE:
            return 0;
        case D3DTOP_SELECTARG1:
        case D3DTOP_PREMODULATE:
            return 2;
        case D3DTOP_SELECTARG2:
            return 4;
        case D3DTOP_MULTIPLYADD:
        case D3DTOP_LERP:
            return 7;
        default:
            return 6;
    }
}

auto Decode(const FixedFunctionKey& key) -> Config
{
    auto config = Config{};
    std::memcpy(&config, key.data(), std::min(sizeof(config), key.size() * sizeof(uint32_t)));
    return config;
}

} // namespace ff

auto KeyFor(const DeviceState& state, DWORD fvf) -> FixedFunctionKey
{
    static_assert(sizeof(Config) % sizeof(uint32_t) == 0);
    const auto& rs       = state.renderStates;
    const auto  vertices = fvf != 0 && !Pretransformed(fvf);

    auto config = Config{};
    config.fvf  = fvf & ~D3DFVF_PSIZE;
    if (vertices)
    {
        config.normalize   = (fvf & D3DFVF_NORMAL) && rs[D3DRS_NORMALIZENORMALS];
        config.localViewer = rs[D3DRS_LOCALVIEWER] != 0;
    }

    config.specular = rs[D3DRS_SPECULARENABLE] != 0;
    if (vertices && rs[D3DRS_LIGHTING])
    {
        const auto colorVertex = rs[D3DRS_COLORVERTEX] != 0;
        config.lighting        = 1;
        config.sources         = { Source(rs[D3DRS_DIFFUSEMATERIALSOURCE], fvf, colorVertex),
                                   Source(rs[D3DRS_AMBIENTMATERIALSOURCE], fvf, colorVertex),
                                   Source(rs[D3DRS_SPECULARMATERIALSOURCE], fvf, colorVertex),
                                   Source(rs[D3DRS_EMISSIVEMATERIALSOURCE], fvf, colorVertex) };
        for (const auto* const light : EnabledLights(state))
        {
            config.lights[config.lightCount++] = light->Type;
        }
    }

    if (rs[D3DRS_FOGENABLE] && fvf != 0)
    {
        config.fog      = 1;
        config.tableFog = rs[D3DRS_FOGTABLEMODE];
        if (config.tableFog == D3DFOG_NONE && vertices)
        {
            config.vertexFog = rs[D3DRS_FOGVERTEXMODE];
            config.rangeFog  = config.vertexFog != D3DFOG_NONE && rs[D3DRS_RANGEFOGENABLE];
        }
    }

    for (uint32_t i = 0; i < 8; ++i)
    {
        const auto& ts    = state.textureStates[i];
        auto&       stage = config.stages[i];
        if (state.pixelShader != 0)
        {
            // A pixel shader samples whatever is bound, with the coordinates this stage routes.
            if (state.textures[i])
            {
                stage.texture = KindOf(state.textures[i].Get());
            }
            else
            {
                stage.texture = kNoTexture;
            }

            RouteCoordinates(stage, ts);
            config.stageCount = i + 1;
            continue;
        }

        if (ts[D3DTSS_COLOROP] == D3DTOP_DISABLE)
        {
            break;
        }

        constexpr auto kArgMask = DWORD{ D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE };
        stage.colorOp           = ts[D3DTSS_COLOROP];
        stage.alphaOp           = ts[D3DTSS_ALPHAOP];
        stage.colorArgs         = { ts[D3DTSS_COLORARG0] & kArgMask, ts[D3DTSS_COLORARG1] & kArgMask, ts[D3DTSS_COLORARG2] & kArgMask };
        stage.alphaArgs         = { ts[D3DTSS_ALPHAARG0] & kArgMask, ts[D3DTSS_ALPHAARG1] & kArgMask, ts[D3DTSS_ALPHAARG2] & kArgMask };
        stage.resultTemp        = (ts[D3DTSS_RESULTARG] & D3DTA_SELECTMASK) == D3DTA_TEMP;
        if (ReadsTexture(stage.colorOp, stage.colorArgs) || ReadsTexture(stage.alphaOp, stage.alphaArgs))
        {
            stage.texture = KindOf(state.textures[i].Get());
        }

        RouteCoordinates(stage, ts);
        config.stageCount = i + 1;
    }

    auto key = FixedFunctionKey(sizeof(config) / sizeof(uint32_t));
    std::memcpy(key.data(), &config, sizeof(config));
    return key;
}

void FixedFunctionUniforms(unsigned program, const DeviceState& state, DWORD fvf)
{
    auto&       locations = LocationsFor(program);
    const auto& rs        = state.renderStates;

    auto worldView = D3DXMATRIX{};
    D3DXMatrixMultiply(&worldView, static_cast<const D3DXMATRIX*>(&state.transforms[D3DTS_WORLD]), static_cast<const D3DXMATRIX*>(&state.transforms[D3DTS_VIEW]));
    const auto& projection   = state.transforms[D3DTS_PROJECTION];
    const auto  normalMatrix = NormalMatrix(worldView);
    if (locations.worldView.Changed(&worldView.m[0][0], sizeof(D3DMATRIX)))
    {
        glUniformMatrix4fv(locations.worldView.location, 1, GL_FALSE, &worldView.m[0][0]);
    }

    if (locations.projection.Changed(projection))
    {
        glUniformMatrix4fv(locations.projection.location, 1, GL_FALSE, &projection.m[0][0]);
    }

    if (locations.normalMatrix.Changed(normalMatrix))
    {
        glUniformMatrix3fv(locations.normalMatrix.location, 1, GL_FALSE, normalMatrix.data());
    }

    const auto  lights   = EnabledLights(state);
    auto        params   = std::vector<std::array<float, 4>>(kLightParamsStart + kLightParams * lights.size());
    const auto& viewport = state.viewport;
    params[kViewport]    = { float(viewport.X), float(viewport.Y), 2.0f / std::max<DWORD>(viewport.Width, 1), 2.0f / std::max<DWORD>(viewport.Height, 1) };
    // Table fog runs on eye distance (w) unless the projection is affine, as D3D8 decides from the matrix.
    const auto affine       = projection._14 == 0.0f && projection._24 == 0.0f && projection._34 == 0.0f && projection._44 == 1.0f;
    const auto depthRange   = viewport.MaxZ - viewport.MinZ;
    const auto inverseDepth = [&]() -> float
    {
        if (depthRange != 0.0f)
        {
            return 1.0f / depthRange;
        }

        return 1.0f;
    }();

    const auto depthFromW = [&]() -> float
    {
        if (affine)
        {
            return 0.0f;
        }

        return 1.0f;
    }();

    params[kDepth]       = { viewport.MinZ, inverseDepth, depthFromW, state.material.Power };
    const auto fogStart  = FloatState(state, D3DRS_FOGSTART);
    const auto fogEnd    = FloatState(state, D3DRS_FOGEND);
    params[kFogParams]   = { fogStart, fogEnd, InverseSpan(fogStart, fogEnd), FloatState(state, D3DRS_FOGDENSITY) };
    params[kAmbient]     = Rgba(rs[D3DRS_AMBIENT]);
    const auto& material = state.material;
    for (const auto [slot, color] :
         { std::pair{ kMaterialDiffuse, &material.Diffuse }, std::pair{ kMaterialAmbient, &material.Ambient }, std::pair{ kMaterialSpecular, &material.Specular }, std::pair{ kMaterialEmissive, &material.Emissive } })
    {
        params[slot] = { color->r, color->g, color->b, color->a };
    }

    const auto& view = state.transforms[D3DTS_VIEW];
    for (size_t i = 0; i < lights.size(); ++i)
    {
        const auto& light    = *lights[i];
        auto* const out      = &params[kLightParamsStart + kLightParams * i];
        out[kLightDiffuse]   = { light.Diffuse.r, light.Diffuse.g, light.Diffuse.b, light.Diffuse.a };
        out[kLightSpecular]  = { light.Specular.r, light.Specular.g, light.Specular.b, light.Specular.a };
        out[kLightAmbient]   = { light.Ambient.r, light.Ambient.g, light.Ambient.b, light.Ambient.a };
        out[kLightPosition]  = TransformPoint(light.Position, view, 1.0f);
        auto       direction = TransformPoint(light.Direction, view, 0.0f);
        const auto length    = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (length > 0.0f)
        {
            for (auto& value : direction)
            {
                value /= length;
            }
        }

        out[kLightDirection]   = direction;
        out[kLightAttenuation] = { light.Attenuation0, light.Attenuation1, light.Attenuation2, light.Range };
        const auto cosTheta    = std::cos(light.Theta / 2.0f);
        const auto cosPhi      = std::cos(light.Phi / 2.0f);
        out[kLightSpot]        = { cosTheta, cosPhi, InverseSpan(cosPhi, cosTheta), light.Falloff };
    }

    if (locations.params.Changed(params.data(), params.size() * sizeof(params[0])))
    {
        glUniform4fv(locations.params.location, GLsizei(params.size()), params[0].data());
    }

    auto textureMatrices = std::array<D3DMATRIX, 8>{};
    for (int i = 0; i < 8; ++i)
    {
        textureMatrices[i] = state.transforms[D3DTS_TEXTURE0 + i];
    }

    if (locations.textureMatrices.Changed(textureMatrices))
    {
        glUniformMatrix4fv(locations.textureMatrices.location, 8, GL_FALSE, &textureMatrices[0].m[0][0]);
    }

    const auto textureFactor = Rgba(rs[D3DRS_TEXTUREFACTOR]);
    if (locations.textureFactor.Changed(textureFactor))
    {
        glUniform4fv(locations.textureFactor.location, 1, textureFactor.data());
    }
}

} // namespace gl
