#pragma once
#include <windows.h>

namespace tmloader
{

// TM's single vertex layout: XYZRHW, diffuse, specular, one UV pair (32 bytes).
constexpr DWORD gameVertexFormat = 0x1c4;
HRESULT         createScaleShader(void* device, DWORD* shader);
HRESULT         setScaleConstants(void* device, float factor);
void            deleteScaleShader(void* device, DWORD& shader);
float           activeRenderScale();

} // namespace tmloader
