#pragma once
#include <windows.h>

#include <d3d9.h>
#include <filesystem>
#include <span>
#include <wrl/client.h>

namespace tmloader
{

struct Image
{
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture;
    UINT                                      width  = 0;
    UINT                                      height = 0;

    explicit operator bool() const noexcept;
};

// Decodes a PNG or JPEG (via WIC) into a managed texture; empty on any failure.
auto loadImage(IDirect3DDevice9* device, std::span<const std::byte> encoded) -> Image;
auto loadImage(IDirect3DDevice9* device, const std::filesystem::path& file) -> Image;

} // namespace tmloader
