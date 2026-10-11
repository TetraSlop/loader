#include "graphics/d3d8_abi.h"
#include "platform/patch.h"
#include "textures/texture_replacements.h"
#include "ui/window.h"

namespace tmloader
{
namespace
{

using DrawFn          = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT);
using DrawIndexedFn   = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT, UINT, UINT);
using DrawUPFn        = HRESULT(WINAPI*)(void*, DWORD, UINT, const void*, UINT);
using DrawIndexedUPFn = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT, UINT, const void*, DWORD, const void*, UINT);

DrawFn          originalDraw;
DrawIndexedFn   originalDrawIndexed;
DrawUPFn        originalDrawUP;
DrawIndexedUPFn originalDrawIndexedUP;

auto bindReplacement(void* device) -> TextureDrawBinding
{
    const auto settings = currentWindowSettings();
    return TextureDrawBinding(device, settings.hdArtwork, settings.hdFonts);
}

auto WINAPI hookedDrawPrimitive(void* device, DWORD type, UINT start, UINT count) -> HRESULT
{
    const auto binding = bindReplacement(device);
    return originalDraw(device, type, start, count);
}

auto WINAPI hookedDrawIndexedPrimitive(void* device, DWORD type, UINT minIndex, UINT vertices, UINT start, UINT count) -> HRESULT
{
    const auto binding = bindReplacement(device);
    return originalDrawIndexed(device, type, minIndex, vertices, start, count);
}

auto WINAPI hookedDrawPrimitiveUP(void* device, DWORD type, UINT count, const void* data, UINT stride) -> HRESULT
{
    const auto binding = bindReplacement(device);
    return originalDrawUP(device, type, count, data, stride);
}

auto WINAPI hookedDrawIndexedPrimitiveUP(void* device, DWORD type, UINT minIndex, UINT vertices, UINT count, const void* indices, DWORD indexFormat, const void* data, UINT stride) -> HRESULT
{
    const auto binding = bindReplacement(device);
    return originalDrawIndexedUP(device, type, minIndex, vertices, count, indices, indexFormat, data, stride);
}

} // namespace

void hookTextureDraws(void** deviceVtable)
{
    if (!texturePackCount())
    {
        return;
    }

    hook(originalDraw, deviceVtable[d3d8::DrawPrimitive], hookedDrawPrimitive);
    hook(originalDrawIndexed, deviceVtable[d3d8::DrawIndexedPrimitive], hookedDrawIndexedPrimitive);
    hook(originalDrawUP, deviceVtable[d3d8::DrawPrimitiveUP], hookedDrawPrimitiveUP);
    hook(originalDrawIndexedUP, deviceVtable[d3d8::DrawIndexedPrimitiveUP], hookedDrawIndexedPrimitiveUP);
}

} // namespace tmloader
