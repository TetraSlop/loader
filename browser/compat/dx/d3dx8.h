// The D3DX declarations gl/ and d3dx8_pixels.cpp use, in place of the DirectX 8 SDK's d3dx8.h. The game links its
// own D3DX, so nothing else is needed.
#pragma once

#include <d3d8.h>

struct D3DXMATRIX : D3DMATRIX
{
};

D3DXMATRIX* WINAPI D3DXMatrixMultiply(D3DXMATRIX* out, const D3DXMATRIX* m1, const D3DXMATRIX* m2);

#define D3DX_FILTER_NONE  (1 << 0)
#define D3DX_FILTER_POINT (2 << 0)
