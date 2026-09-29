#include "ui/image.h"
#include <fstream>
#include <iterator>
#include <vector>
#include <wincodec.h>

namespace tmloader
{
namespace
{

using Microsoft::WRL::ComPtr;

auto decode(std::span<const std::byte> encoded, UINT& width, UINT& height) -> std::vector<BYTE>
{
    ComPtr<IWICImagingFactory>    factory;
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   converter;
    auto* const                   bytes = reinterpret_cast<BYTE*>(const_cast<std::byte*>(encoded.data()));
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(bytes, static_cast<DWORD>(encoded.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192)
    {
        return {};
    }

    auto pixels = std::vector<BYTE>(size_t{ width } * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
    {
        return {};
    }

    return pixels;
}

} // namespace

auto loadImage(IDirect3DDevice9* device, std::span<const std::byte> encoded) -> Image
{
    auto       image  = Image{};
    const auto pixels = decode(encoded, image.width, image.height);
    if (pixels.empty() ||
        FAILED(device->CreateTexture(image.width, image.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, image.texture.GetAddressOf(), nullptr)))
    {
        return {};
    }

    auto locked = D3DLOCKED_RECT{};
    if (FAILED(image.texture->LockRect(0, &locked, nullptr, 0)))
    {
        return {};
    }

    for (UINT y = 0; y < image.height; ++y)
    {
        std::memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, pixels.data() + size_t{ y } * image.width * 4, size_t{ image.width } * 4);
    }

    image.texture->UnlockRect(0);
    return image;
}

auto loadImage(IDirect3DDevice9* device, const std::filesystem::path& file) -> Image
{
    auto input = std::ifstream(file, std::ios::binary);
    if (!input)
    {
        return {};
    }

    const auto bytes = std::vector<char>(std::istreambuf_iterator<char>(input), {});
    return loadImage(device, std::as_bytes(std::span(bytes)));
}

} // namespace tmloader
