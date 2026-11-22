#include "d3d8_null.h"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace nulldx
{

namespace
{

// A DirectX 8.1 card with hardware T&L, vs.1.1, ps.1.4 and no palettized textures, so the client takes its full path.
auto MakeCaps() -> D3DCAPS8
{
    auto caps           = D3DCAPS8{};
    caps.DeviceType     = D3DDEVTYPE_HAL;
    caps.AdapterOrdinal = 0;
    caps.Caps           = D3DCAPS_READ_SCANLINE;
    // Direct3D8Manager reads gamma support from D3DCAPS2_FULLSCREENGAMMA, 0x20000 like D3DDEVCAPS_CANBLTSYSTONONLOCAL.
    caps.Caps2                 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES;
    caps.Caps3                 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD;
    caps.PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
    caps.CursorCaps            = D3DCURSORCAPS_COLOR;
    caps.DevCaps               = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_HWRASTERIZATION;
    caps.PrimitiveMiscCaps     = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP;
    caps.RasterCaps            = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_COLORPERSPECTIVE;
    caps.ZCmpCaps              = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    caps.AlphaCmpCaps          = caps.ZCmpCaps;
    caps.SrcBlendCaps          = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA;
    caps.DestBlendCaps         = caps.SrcBlendCaps;
    caps.ShadeCaps             = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
    caps.TextureCaps           = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPCUBEMAP;
    caps.TextureFilterCaps     = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    caps.CubeTextureFilterCaps = caps.TextureFilterCaps;
    caps.TextureAddressCaps    = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    caps.LineCaps              = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
    // MojiDraw only uses its high resolution font atlas from 2048x4096 up.
    caps.MaxTextureWidth           = 4096;
    caps.MaxTextureHeight          = 4096;
    caps.MaxTextureRepeat          = 8192;
    caps.MaxTextureAspectRatio     = 4096;
    caps.MaxAnisotropy             = 16;
    caps.MaxVertexW                = 1e10f;
    caps.GuardBandLeft             = -1e8f;
    caps.GuardBandTop              = -1e8f;
    caps.GuardBandRight            = 1e8f;
    caps.GuardBandBottom           = 1e8f;
    caps.ExtentsAdjust             = 0.0f;
    caps.StencilCaps               = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
    caps.FVFCaps                   = 8 | D3DFVFCAPS_PSIZE;
    caps.TextureOpCaps             = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_PREMODULATE | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA | D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP;
    caps.MaxTextureBlendStages     = 8;
    caps.MaxSimultaneousTextures   = 4;
    caps.VertexProcessingCaps      = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    caps.MaxActiveLights           = 8;
    caps.MaxUserClipPlanes         = 0;
    caps.MaxVertexBlendMatrices    = 4;
    caps.MaxVertexBlendMatrixIndex = 0;
    caps.MaxPointSize              = 64.0f;
    caps.MaxPrimitiveCount         = 0xFFFFF;
    caps.MaxVertexIndex            = 0xFFFFF;
    caps.MaxStreams                = kStreams;
    caps.MaxStreamStride           = 255;
    caps.VertexShaderVersion       = D3DVS_VERSION(1, 1);
    caps.MaxVertexShaderConst      = kVertexConstants;
    caps.PixelShaderVersion        = D3DPS_VERSION(1, 4);
    caps.MaxPixelShaderValue       = 8.0f;
    return caps;
}

// DirectXInitializer keeps the X8R8G8B8 modes from 640x480 to 2048x2048.
constexpr UINT kModeSizes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 900 }, { 1920, 1080 } };

auto Mode(UINT index) -> D3DDISPLAYMODE
{
    return { kModeSizes[index][0], kModeSizes[index][1], 60, D3DFMT_X8R8G8B8 };
}

} // namespace

auto NullDirect3D::DesktopMode() -> const D3DDISPLAYMODE&
{
    static const auto mode = Mode(std::size(kModeSizes) - 1);
    return mode;
}

HRESULT NullDirect3D::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier)
{
    if (adapter != 0 || identifier == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    std::memset(identifier, 0, sizeof(*identifier));
    std::snprintf(identifier->Driver, sizeof(identifier->Driver), "nulldx");
    std::snprintf(identifier->Description, sizeof(identifier->Description), "Kestrel null Direct3D 8 device");
    // Neither the AMD nor the Intel vendor id, which select driver workarounds in the client.
    identifier->VendorId  = 0x10DE;
    identifier->DeviceId  = 0x0250;
    identifier->WHQLLevel = 1;
    return D3D_OK;
}

UINT NullDirect3D::GetAdapterModeCount(UINT adapter)
{
    if (adapter != 0)
    {
        return 0;
    }

    return std::size(kModeSizes);
}

HRESULT NullDirect3D::EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* displayMode)
{
    if (adapter != 0 || mode >= std::size(kModeSizes))
    {
        return D3DERR_INVALIDCALL;
    }

    return Store(displayMode, Mode(mode), D3DERR_INVALIDCALL);
}

HRESULT NullDirect3D::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* displayMode)
{
    if (adapter != 0)
    {
        return D3DERR_INVALIDCALL;
    }

    return Store(displayMode, DesktopMode(), D3DERR_INVALIDCALL);
}

HRESULT NullDirect3D::CheckDeviceType(
    UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat, D3DFORMAT backBufferFormat, BOOL windowed)
{
    if (adapter != 0)
    {
        return D3DERR_INVALIDCALL;
    }

    return D3D_OK;
}

HRESULT NullDirect3D::CheckDeviceFormat(
    UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage, D3DRESOURCETYPE resourceType, D3DFORMAT format)
{
    if (adapter != 0)
    {
        return D3DERR_INVALIDCALL;
    }

    const auto available = [&]() -> bool
    {
        if ((usage & D3DUSAGE_DEPTHSTENCIL) != 0)
        {
            return IsDepthFormat(format);
        }

        // CreateVolumeTexture makes none
        if (resourceType == D3DRTYPE_VOLUMETEXTURE)
        {
            return false;
        }

        if (resourceType == D3DRTYPE_TEXTURE || resourceType == D3DRTYPE_CUBETEXTURE)
        {
            return IsTextureFormat(format);
        }

        return LayoutOf(format).blockBytes != 0;
    }();

    if (!available)
    {
        return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
}

HRESULT NullDirect3D::CheckDeviceMultiSampleType(
    UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE multiSample)
{
    if (adapter != 0)
    {
        return D3DERR_INVALIDCALL;
    }

    if (multiSample != D3DMULTISAMPLE_NONE)
    {
        return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
}

HRESULT NullDirect3D::CheckDepthStencilMatch(
    UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat)
{
    if (adapter != 0)
    {
        return D3DERR_INVALIDCALL;
    }

    if (!IsDepthFormat(depthStencilFormat))
    {
        return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
}

HRESULT NullDirect3D::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps)
{
    if (adapter != 0 || caps == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *caps            = MakeCaps();
    caps->DeviceType = type;
    return D3D_OK;
}

auto CompletePresentParameters(D3DPRESENT_PARAMETERS& params) -> void
{
    if (params.BackBufferWidth == 0)
    {
        params.BackBufferWidth = NullDirect3D::DesktopMode().Width;
    }

    if (params.BackBufferHeight == 0)
    {
        params.BackBufferHeight = NullDirect3D::DesktopMode().Height;
    }

    if (params.BackBufferFormat == D3DFMT_UNKNOWN)
    {
        params.BackBufferFormat = NullDirect3D::DesktopMode().Format;
    }

    if (params.BackBufferCount == 0)
    {
        params.BackBufferCount = 1;
    }
}

// No window is needed: nothing is shown, and focusWindow is only kept for GetCreationParameters.
HRESULT NullDirect3D::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice8** device)
{
    if (device == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *device = nullptr;
    if (adapter != 0 || params == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    CompletePresentParameters(*params);
    *device = new NullDevice(this, { adapter, type, focusWindow, behaviorFlags }, *params);
    return D3D_OK;
}

} // namespace nulldx

IDirect3D8* WINAPI Direct3DCreate8(UINT sdkVersion)
{
    if (sdkVersion != D3D_SDK_VERSION)
    {
        return nullptr;
    }

    return new nulldx::NullDirect3D();
}
