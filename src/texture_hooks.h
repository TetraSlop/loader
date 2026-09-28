#pragma once
#include <windows.h>

namespace tmloader
{

// Statically linked D3DX8 texture upload ABI; all fifteen arguments are retained.
using TextureUploadFn = HRESULT(WINAPI*)(void* device, const void* source, UINT bytes, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey, void* sourceInfo, void* palette, void** output);

// Install before native game initialization, while no thread can execute these
// calls. Unsupported code or memory protection leaves all original calls intact.
auto installTextureCallHooks(BYTE* image, TextureUploadFn replacement) -> TextureUploadFn;

} // namespace tmloader
