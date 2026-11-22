// The state lists and defaults follow D3D8 as Wine's d3d8 documents it.
#include "d3d8_null.h"

#include <array>
#include <cstring>

namespace nulldx
{

namespace
{

auto FloatBits(float value) -> DWORD
{
    auto bits = DWORD{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

constexpr auto kPixelRenderStates = std::array{
    D3DRS_ZENABLE,
    D3DRS_FILLMODE,
    D3DRS_SHADEMODE,
    D3DRS_LINEPATTERN,
    D3DRS_ZWRITEENABLE,
    D3DRS_ALPHATESTENABLE,
    D3DRS_LASTPIXEL,
    D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,
    D3DRS_ZFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE,
    D3DRS_FOGSTART,
    D3DRS_FOGEND,
    D3DRS_FOGDENSITY,
    D3DRS_EDGEANTIALIAS,
    D3DRS_ALPHABLENDENABLE,
    D3DRS_ZBIAS,
    D3DRS_STENCILENABLE,
    D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL,
    D3DRS_STENCILPASS,
    D3DRS_STENCILFUNC,
    D3DRS_STENCILREF,
    D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK,
    D3DRS_TEXTUREFACTOR,
    D3DRS_WRAP0,
    D3DRS_WRAP1,
    D3DRS_WRAP2,
    D3DRS_WRAP3,
    D3DRS_WRAP4,
    D3DRS_WRAP5,
    D3DRS_WRAP6,
    D3DRS_WRAP7,
    D3DRS_COLORWRITEENABLE,
    D3DRS_BLENDOP,
};

constexpr auto kVertexRenderStates = std::array{
    D3DRS_SHADEMODE,
    D3DRS_SPECULARENABLE,
    D3DRS_CULLMODE,
    D3DRS_FOGENABLE,
    D3DRS_FOGCOLOR,
    D3DRS_FOGTABLEMODE,
    D3DRS_FOGSTART,
    D3DRS_FOGEND,
    D3DRS_FOGDENSITY,
    D3DRS_RANGEFOGENABLE,
    D3DRS_AMBIENT,
    D3DRS_COLORVERTEX,
    D3DRS_FOGVERTEXMODE,
    D3DRS_CLIPPING,
    D3DRS_LIGHTING,
    D3DRS_NORMALIZENORMALS,
    D3DRS_LOCALVIEWER,
    D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE,
    D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_VERTEXBLEND,
    D3DRS_CLIPPLANEENABLE,
    D3DRS_SOFTWAREVERTEXPROCESSING,
    D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSCALE_A,
    D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C,
    D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE,
    D3DRS_PATCHSEGMENTS,
    D3DRS_POINTSIZE_MAX,
    D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_TWEENFACTOR,
    D3DRS_POSITIONORDER,
    D3DRS_NORMALORDER,
};

constexpr auto IsVertexTextureState(DWORD type) -> bool
{
    return type == D3DTSS_TEXCOORDINDEX || type == D3DTSS_TEXTURETRANSFORMFLAGS;
}

} // namespace

auto DefaultState(const D3DPRESENT_PARAMETERS& params) -> DeviceState
{
    const auto zEnable = [&]() -> DWORD
    {
        if (params.EnableAutoDepthStencil)
        {
            return D3DZB_TRUE;
        }

        return D3DZB_FALSE;
    }();

    auto  state                      = DeviceState{};
    auto& rs                         = state.renderStates;
    rs[D3DRS_ZENABLE]                = zEnable;
    rs[D3DRS_FILLMODE]               = D3DFILL_SOLID;
    rs[D3DRS_SHADEMODE]              = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE]           = TRUE;
    rs[D3DRS_LASTPIXEL]              = TRUE;
    rs[D3DRS_SRCBLEND]               = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND]              = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE]               = D3DCULL_CCW;
    rs[D3DRS_ZFUNC]                  = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAFUNC]              = D3DCMP_ALWAYS;
    rs[D3DRS_FOGTABLEMODE]           = D3DFOG_NONE;
    rs[D3DRS_FOGSTART]               = FloatBits(0.0f);
    rs[D3DRS_FOGEND]                 = FloatBits(1.0f);
    rs[D3DRS_FOGDENSITY]             = FloatBits(1.0f);
    rs[D3DRS_STENCILFAIL]            = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILZFAIL]           = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILPASS]            = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC]            = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK]            = 0xFFFFFFFF;
    rs[D3DRS_STENCILWRITEMASK]       = 0xFFFFFFFF;
    rs[D3DRS_TEXTUREFACTOR]          = 0xFFFFFFFF;
    rs[D3DRS_CLIPPING]               = TRUE;
    rs[D3DRS_LIGHTING]               = TRUE;
    rs[D3DRS_FOGVERTEXMODE]          = D3DFOG_NONE;
    rs[D3DRS_COLORVERTEX]            = TRUE;
    rs[D3DRS_LOCALVIEWER]            = TRUE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE]  = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_AMBIENTMATERIALSOURCE]  = D3DMCS_MATERIAL;
    rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    rs[D3DRS_VERTEXBLEND]            = D3DVBF_DISABLE;
    rs[D3DRS_POINTSIZE]              = FloatBits(1.0f);
    rs[D3DRS_POINTSIZE_MIN]          = FloatBits(0.0f);
    rs[D3DRS_POINTSCALE_A]           = FloatBits(1.0f);
    rs[D3DRS_POINTSCALE_B]           = FloatBits(0.0f);
    rs[D3DRS_POINTSCALE_C]           = FloatBits(0.0f);
    rs[D3DRS_MULTISAMPLEANTIALIAS]   = TRUE;
    rs[D3DRS_MULTISAMPLEMASK]        = 0xFFFFFFFF;
    rs[D3DRS_PATCHEDGESTYLE]         = D3DPATCHEDGE_DISCRETE;
    rs[D3DRS_PATCHSEGMENTS]          = FloatBits(1.0f);
    rs[D3DRS_POINTSIZE_MAX]          = FloatBits(64.0f);
    rs[D3DRS_COLORWRITEENABLE]       = 0x0F;
    rs[D3DRS_TWEENFACTOR]            = FloatBits(0.0f);
    rs[D3DRS_BLENDOP]                = D3DBLENDOP_ADD;
    rs[D3DRS_POSITIONORDER]          = D3DORDER_CUBIC;
    rs[D3DRS_NORMALORDER]            = D3DORDER_LINEAR;

    for (DWORD stage = 0; stage < kTextureStages; ++stage)
    {
        auto& ts                         = state.textureStates[stage];
        ts[D3DTSS_COLOROP]               = D3DTOP_DISABLE;
        ts[D3DTSS_COLORARG1]             = D3DTA_TEXTURE;
        ts[D3DTSS_COLORARG2]             = D3DTA_CURRENT;
        ts[D3DTSS_ALPHAOP]               = D3DTOP_DISABLE;
        ts[D3DTSS_ALPHAARG1]             = D3DTA_TEXTURE;
        ts[D3DTSS_ALPHAARG2]             = D3DTA_CURRENT;
        ts[D3DTSS_TEXCOORDINDEX]         = stage;
        ts[D3DTSS_ADDRESSU]              = D3DTADDRESS_WRAP;
        ts[D3DTSS_ADDRESSV]              = D3DTADDRESS_WRAP;
        ts[D3DTSS_ADDRESSW]              = D3DTADDRESS_WRAP;
        ts[D3DTSS_MAGFILTER]             = D3DTEXF_POINT;
        ts[D3DTSS_MINFILTER]             = D3DTEXF_POINT;
        ts[D3DTSS_MIPFILTER]             = D3DTEXF_NONE;
        ts[D3DTSS_MAXANISOTROPY]         = 1;
        ts[D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
        ts[D3DTSS_COLORARG0]             = D3DTA_CURRENT;
        ts[D3DTSS_ALPHAARG0]             = D3DTA_CURRENT;
        ts[D3DTSS_RESULTARG]             = D3DTA_CURRENT;
    }

    state.textureStates[0][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    state.textureStates[0][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;

    for (auto& matrix : state.transforms)
    {
        matrix     = {};
        matrix._11 = matrix._22 = matrix._33 = matrix._44 = 1.0f;
    }

    state.viewport = { 0, 0, params.BackBufferWidth, params.BackBufferHeight, 0.0f, 1.0f };
    return state;
}

auto MaskFor(D3DSTATEBLOCKTYPE type, const DeviceState& state) -> StateMask
{
    auto       mask = StateMask{};
    const auto all  = type == D3DSBT_ALL;
    if (all)
    {
        mask.renderStates.set();
        mask.textureStates.set();
        mask.textures.set();
        mask.transforms.set();
        mask.streams.set();
        mask.material = mask.viewport = mask.indices = mask.texturePalette = true;
    }

    if (all || type == D3DSBT_PIXELSTATE)
    {
        for (const auto rs : kPixelRenderStates)
        {
            mask.renderStates.set(rs);
        }

        for (DWORD stage = 0; stage < kTextureStages; ++stage)
        {
            for (DWORD ts = 0; ts < kTextureStageStates; ++ts)
            {
                if (!IsVertexTextureState(ts))
                {
                    mask.textureStates.set(stage * kTextureStageStates + ts);
                }
            }
        }

        mask.pixelShader = true;
        mask.pixelConstants.set();
    }

    if (all || type == D3DSBT_VERTEXSTATE)
    {
        for (const auto rs : kVertexRenderStates)
        {
            mask.renderStates.set(rs);
        }

        for (DWORD stage = 0; stage < kTextureStages; ++stage)
        {
            mask.textureStates.set(stage * kTextureStageStates + D3DTSS_TEXCOORDINDEX);
            mask.textureStates.set(stage * kTextureStageStates + D3DTSS_TEXTURETRANSFORMFLAGS);
        }

        for (const auto& light : state.lights)
        {
            mask.lights.insert(light.first);
        }

        mask.vertexShader = true;
        mask.vertexConstants.set();
    }

    return mask;
}

auto CopyMasked(const DeviceState& from, DeviceState& to, const StateMask& mask) -> void
{
    for (DWORD i = 0; i < kRenderStates; ++i)
    {
        if (mask.renderStates[i])
        {
            to.renderStates[i] = from.renderStates[i];
        }
    }

    for (DWORD stage = 0; stage < kTextureStages; ++stage)
    {
        for (DWORD ts = 0; ts < kTextureStageStates; ++ts)
        {
            if (mask.textureStates[stage * kTextureStageStates + ts])
            {
                to.textureStates[stage][ts] = from.textureStates[stage][ts];
            }
        }
    }

    for (DWORD i = 0; i < kTextureStages; ++i)
    {
        if (mask.textures[i])
        {
            to.textures[i] = from.textures[i];
        }
    }

    for (DWORD i = 0; i < kTransforms; ++i)
    {
        if (mask.transforms[i])
        {
            to.transforms[i] = from.transforms[i];
        }
    }

    for (const auto index : mask.lights)
    {
        const auto light = from.lights.find(index);
        if (light != from.lights.end())
        {
            to.lights[index] = light->second;
        }
    }

    for (DWORD i = 0; i < kVertexConstants; ++i)
    {
        if (mask.vertexConstants[i])
        {
            to.vertexConstants[i] = from.vertexConstants[i];
        }
    }

    for (DWORD i = 0; i < kPixelConstants; ++i)
    {
        if (mask.pixelConstants[i])
        {
            to.pixelConstants[i] = from.pixelConstants[i];
        }
    }

    for (DWORD i = 0; i < kStreams; ++i)
    {
        if (mask.streams[i])
        {
            to.streams[i] = from.streams[i];
        }
    }

    if (mask.material)
    {
        to.material = from.material;
    }

    if (mask.viewport)
    {
        to.viewport = from.viewport;
    }

    if (mask.vertexShader)
    {
        to.vertexShader = from.vertexShader;
    }

    if (mask.pixelShader)
    {
        to.pixelShader = from.pixelShader;
    }

    if (mask.indices)
    {
        to.indices         = from.indices;
        to.baseVertexIndex = from.baseVertexIndex;
    }

    if (mask.texturePalette)
    {
        to.texturePalette = from.texturePalette;
    }
}

} // namespace nulldx
