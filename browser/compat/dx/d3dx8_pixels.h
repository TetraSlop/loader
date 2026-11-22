// Pixel conversion for D3DX's surface loaders: every format goes through 32-bit ARGB.
#pragma once

#include <d3d8.h>

#include <cstdint>
#include <vector>

namespace nulldx
{

// ARGB pixels, rows top to bottom.
struct Image
{
    UINT                  width  = 0;
    UINT                  height = 0;
    std::vector<uint32_t> pixels;
};

// bits is the surface's first byte; fails on formats D3DX cannot read (bump maps, depth).
auto Decode(D3DFORMAT format, const BYTE* bits, UINT pitch, const PALETTEENTRY* palette, const RECT& rect, Image& image) -> bool;

// bits points at the block or pixel of the destination rectangle's top left corner.
auto Encode(D3DFORMAT format, BYTE* bits, UINT pitch, const Image& image) -> bool;

// FILTER_NONE crops or pads with transparent black, POINT takes the nearest pixel, every other filter box-averages.
auto Resample(const Image& source, UINT width, UINT height, DWORD filter) -> Image;

} // namespace nulldx
