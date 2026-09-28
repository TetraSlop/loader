#pragma once
#include <windows.h>

namespace tmloader
{

// TM's single vertex layout: XYZRHW, diffuse, specular, one UV pair (32 bytes).
constexpr DWORD gameVertexFormat = 0x1c4;
auto            createScaleShader(void* device, DWORD* shader) -> HRESULT;
auto            setScaleConstants(void* device, float factor) -> HRESULT;
void            deleteScaleShader(void* device, DWORD& shader);
auto            activeRenderScale() -> float;

} // namespace tmloader
