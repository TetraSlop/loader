#include "d3d8_formats.h"

#include <algorithm>

namespace nulldx
{

auto LayoutOf(D3DFORMAT format) -> FormatLayout
{
    switch (format)
    {
        case D3DFMT_DXT1:
            return { 8, 4 };
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
            return { 16, 4 };
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_G16R16:
        case D3DFMT_X8L8V8U8:
        case D3DFMT_Q8W8V8U8:
        case D3DFMT_V16U16:
        case D3DFMT_W11V11U10:
        case D3DFMT_A2W10V10U10:
        case D3DFMT_D32:
        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
        case D3DFMT_D24X4S4:
        case D3DFMT_INDEX32:
            return { 4, 1 };
        case D3DFMT_R8G8B8:
            return { 3, 1 };
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_A8R3G3B2:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8P8:
        case D3DFMT_A8L8:
        case D3DFMT_V8U8:
        case D3DFMT_L6V5U5:
        case D3DFMT_UYVY:
        case D3DFMT_YUY2:
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D15S1:
        case D3DFMT_D16:
        case D3DFMT_INDEX16:
            return { 2, 1 };
        case D3DFMT_R3G3B2:
        case D3DFMT_A8:
        case D3DFMT_P8:
        case D3DFMT_L8:
        case D3DFMT_A4L4:
            return { 1, 1 };
        default:
            return { 0, 1 };
    }
}

auto RowPitch(D3DFORMAT format, UINT width) -> UINT
{
    const auto layout = LayoutOf(format);
    return (width + layout.blockEdge - 1) / layout.blockEdge * layout.blockBytes;
}

auto BlockRows(D3DFORMAT format, UINT height) -> UINT
{
    const auto layout = LayoutOf(format);
    return (height + layout.blockEdge - 1) / layout.blockEdge;
}

auto LevelCount(UINT width, UINT height, UINT depth, UINT levels) -> UINT
{
    auto full = UINT{ 1 };
    for (auto size = std::max({ width, height, depth }); size > 1; size >>= 1)
    {
        ++full;
    }

    if (levels == 0)
    {
        return full;
    }

    return std::min(levels, full);
}

// Palettes are left out, as on the hardware the client's renderer was written for, so it keeps its own conversion.
auto IsTextureFormat(D3DFORMAT format) -> bool
{
    return LayoutOf(format).blockBytes != 0 && format != D3DFMT_P8 && format != D3DFMT_A8P8 && format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32 && !IsDepthFormat(format);
}

auto IsDepthFormat(D3DFORMAT format) -> bool
{
    switch (format)
    {
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D32:
        case D3DFMT_D15S1:
        case D3DFMT_D24S8:
        case D3DFMT_D16:
        case D3DFMT_D24X8:
        case D3DFMT_D24X4S4:
            return true;
        default:
            return false;
    }
}

auto SurfaceDesc(D3DFORMAT format, DWORD usage, D3DPOOL pool, UINT width, UINT height, D3DMULTISAMPLE_TYPE multiSample) -> D3DSURFACE_DESC
{
    return { format, D3DRTYPE_SURFACE, usage, pool, RowPitch(format, width) * BlockRows(format, height), multiSample, width, height };
}

} // namespace nulldx
