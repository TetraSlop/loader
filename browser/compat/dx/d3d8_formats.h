// Memory layout of Direct3D 8 formats, shared by the null device and D3DX.
#pragma once

#include <d3d8.h>

namespace nulldx
{

// blockEdge is 4 for DXT and 1 otherwise; blockBytes is 0 for formats the null device does not know.
struct FormatLayout
{
    UINT blockBytes;
    UINT blockEdge;
};

auto LayoutOf(D3DFORMAT format) -> FormatLayout;
auto RowPitch(D3DFORMAT format, UINT width) -> UINT;
auto BlockRows(D3DFORMAT format, UINT height) -> UINT;
auto LevelCount(UINT width, UINT height, UINT depth, UINT levels) -> UINT;
auto IsTextureFormat(D3DFORMAT format) -> bool;
auto IsDepthFormat(D3DFORMAT format) -> bool;
auto SurfaceDesc(D3DFORMAT format, DWORD usage, D3DPOOL pool, UINT width, UINT height, D3DMULTISAMPLE_TYPE multiSample = D3DMULTISAMPLE_NONE) -> D3DSURFACE_DESC;

} // namespace nulldx
