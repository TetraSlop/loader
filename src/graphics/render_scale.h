#pragma once
#include <windows.h>

namespace tmloader
{

// The game's only vertex layout: XYZRHW, diffuse, specular, one UV pair (32 bytes).
constexpr DWORD kGameVertexFormat = 0x1c4;

// Above 1x the game's pretransformed vertices are scaled by a small vertex shader.
auto createScaleShader(void* device, DWORD* shader) -> HRESULT;
auto setScaleConstants(void* device, float scale) -> HRESULT;
void deleteScaleShader(void* device, DWORD& shader);
auto activeRenderScale() -> float;

} // namespace tmloader
