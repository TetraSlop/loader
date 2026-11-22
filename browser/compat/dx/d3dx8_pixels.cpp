#include "d3dx8_pixels.h"

#include "d3d8_formats.h"

#include <d3dx8.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace nulldx
{

namespace
{

constexpr auto Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) -> uint32_t
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

constexpr auto Alpha(uint32_t argb) -> uint32_t
{
    return argb >> 24;
}

constexpr auto Red(uint32_t argb) -> uint32_t
{
    return (argb >> 16) & 0xFF;
}

constexpr auto Green(uint32_t argb) -> uint32_t
{
    return (argb >> 8) & 0xFF;
}

constexpr auto Blue(uint32_t argb) -> uint32_t
{
    return argb & 0xFF;
}

// Scales an n-bit channel to 8 bits and back, rounding to nearest.
constexpr auto Widen(uint32_t value, uint32_t bits) -> uint32_t
{
    const auto max = (1u << bits) - 1;
    return (value * 255 + max / 2) / max;
}

constexpr auto Narrow(uint32_t value, uint32_t bits) -> uint32_t
{
    const auto max = (1u << bits) - 1;
    return (value * max + 127) / 255;
}

constexpr auto Luminance(uint32_t argb) -> uint32_t
{
    return (Red(argb) * 77 + Green(argb) * 150 + Blue(argb) * 29 + 128) >> 8;
}

auto Load16(const BYTE* p) -> uint16_t
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

auto Store16(BYTE* p, uint32_t v) -> void
{
    p[0] = static_cast<BYTE>(v);
    p[1] = static_cast<BYTE>(v >> 8);
}

constexpr auto From565(uint32_t v) -> uint32_t
{
    return Argb(0xFF, Widen(v >> 11, 5), Widen((v >> 5) & 0x3F, 6), Widen(v & 0x1F, 5));
}

constexpr auto To565(uint32_t argb) -> uint32_t
{
    return (Narrow(Red(argb), 5) << 11) | (Narrow(Green(argb), 6) << 5) | Narrow(Blue(argb), 5);
}

auto FromPalette(const PALETTEENTRY* palette, BYTE index) -> uint32_t
{
    if (palette == nullptr)
    {
        return Argb(0xFF, index, index, index);
    }

    const auto& entry = palette[index];
    return Argb(entry.peFlags, entry.peRed, entry.peGreen, entry.peBlue);
}

auto ReadPixel(D3DFORMAT format, const BYTE* p, const PALETTEENTRY* palette) -> std::optional<uint32_t>
{
    switch (format)
    {
        case D3DFMT_A8R8G8B8:
            return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        case D3DFMT_X8R8G8B8:
        case D3DFMT_R8G8B8:
            return Argb(0xFF, p[2], p[1], p[0]);
        case D3DFMT_R5G6B5:
            return From565(Load16(p));
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        {
            const auto v = uint32_t{ Load16(p) };
            const auto a = [&]() -> uint32_t
            {
                if (format == D3DFMT_X1R5G5B5 || (v & 0x8000) != 0)
                {
                    return 0xFF;
                }

                return 0;
            }();

            return Argb(a, Widen((v >> 10) & 0x1F, 5), Widen((v >> 5) & 0x1F, 5), Widen(v & 0x1F, 5));
        }
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        {
            const auto v = uint32_t{ Load16(p) };
            const auto a = [&]() -> uint32_t
            {
                if (format == D3DFMT_X4R4G4B4)
                {
                    return 0xFF;
                }

                return Widen(v >> 12, 4);
            }();

            return Argb(a, Widen((v >> 8) & 0xF, 4), Widen((v >> 4) & 0xF, 4), Widen(v & 0xF, 4));
        }
        case D3DFMT_R3G3B2:
            return Argb(0xFF, Widen(p[0] >> 5, 3), Widen((p[0] >> 2) & 7, 3), Widen(p[0] & 3, 2));
        case D3DFMT_A8R3G3B2:
            return Argb(p[1], Widen(p[0] >> 5, 3), Widen((p[0] >> 2) & 7, 3), Widen(p[0] & 3, 2));
        case D3DFMT_A8:
            return Argb(p[0], 0, 0, 0);
        case D3DFMT_L8:
            return Argb(0xFF, p[0], p[0], p[0]);
        case D3DFMT_A8L8:
            return Argb(p[1], p[0], p[0], p[0]);
        case D3DFMT_A4L4:
        {
            const auto l = Widen(p[0] & 0xF, 4);
            return Argb(Widen(p[0] >> 4, 4), l, l, l);
        }
        case D3DFMT_P8:
            return FromPalette(palette, p[0]);
        case D3DFMT_A8P8:
            return (FromPalette(palette, p[0]) & 0x00FFFFFF) | (static_cast<uint32_t>(p[1]) << 24);
        default:
            return std::nullopt;
    }
}

auto WritePixel(D3DFORMAT format, uint32_t argb, BYTE* p) -> bool
{
    const auto a = Alpha(argb);
    const auto r = Red(argb);
    const auto g = Green(argb);
    const auto b = Blue(argb);
    switch (format)
    {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        {
            const auto alpha = [&]() -> uint32_t
            {
                if (format == D3DFMT_X8R8G8B8)
                {
                    return 0xFF;
                }

                return a;
            }();

            p[0] = static_cast<BYTE>(b);
            p[1] = static_cast<BYTE>(g);
            p[2] = static_cast<BYTE>(r);
            p[3] = static_cast<BYTE>(alpha);
            return true;
        }
        case D3DFMT_R8G8B8:
            p[0] = static_cast<BYTE>(b);
            p[1] = static_cast<BYTE>(g);
            p[2] = static_cast<BYTE>(r);
            return true;
        case D3DFMT_R5G6B5:
            Store16(p, To565(argb));
            return true;
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        {
            const auto high = [&]() -> uint32_t
            {
                if (format == D3DFMT_X1R5G5B5 || a >= 0x80)
                {
                    return 0x8000;
                }

                return 0;
            }();

            Store16(p, high | (Narrow(r, 5) << 10) | (Narrow(g, 5) << 5) | Narrow(b, 5));
            return true;
        }
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        {
            const auto alpha = [&]() -> uint32_t
            {
                if (format == D3DFMT_X4R4G4B4)
                {
                    return 0xF;
                }

                return Narrow(a, 4);
            }();

            Store16(p, (alpha << 12) | (Narrow(r, 4) << 8) | (Narrow(g, 4) << 4) | Narrow(b, 4));
            return true;
        }
        case D3DFMT_R3G3B2:
            p[0] = static_cast<BYTE>((Narrow(r, 3) << 5) | (Narrow(g, 3) << 2) | Narrow(b, 2));
            return true;
        case D3DFMT_A8R3G3B2:
            p[0] = static_cast<BYTE>((Narrow(r, 3) << 5) | (Narrow(g, 3) << 2) | Narrow(b, 2));
            p[1] = static_cast<BYTE>(a);
            return true;
        case D3DFMT_A8:
            p[0] = static_cast<BYTE>(a);
            return true;
        case D3DFMT_L8:
            p[0] = static_cast<BYTE>(Luminance(argb));
            return true;
        case D3DFMT_A8L8:
            p[0] = static_cast<BYTE>(Luminance(argb));
            p[1] = static_cast<BYTE>(a);
            return true;
        case D3DFMT_A4L4:
            p[0] = static_cast<BYTE>((Narrow(a, 4) << 4) | Narrow(Luminance(argb), 4));
            return true;
        default:
            return false;
    }
}

constexpr auto Mix(uint32_t a, uint32_t b, uint32_t weightA, uint32_t weightB) -> uint32_t
{
    const auto total = weightA + weightB;
    return Argb(0xFF, (Red(a) * weightA + Red(b) * weightB) / total, (Green(a) * weightA + Green(b) * weightB) / total, (Blue(a) * weightA + Blue(b) * weightB) / total);
}

// The four colours a DXT colour block picks from; the 3-colour mode only exists in DXT1.
auto ColorPalette(uint32_t c0, uint32_t c1, bool threeColor, uint32_t out[4]) -> void
{
    out[0] = From565(c0);
    out[1] = From565(c1);
    if (!threeColor)
    {
        out[2] = Mix(out[0], out[1], 2, 1);
        out[3] = Mix(out[0], out[1], 1, 2);
    }
    else
    {
        out[2] = Mix(out[0], out[1], 1, 1);
        out[3] = 0;
    }
}

auto AlphaPalette(uint32_t a0, uint32_t a1, uint32_t out[8]) -> void
{
    out[0] = a0;
    out[1] = a1;
    if (a0 > a1)
    {
        for (uint32_t i = 1; i < 7; ++i)
        {
            out[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        }
    }
    else
    {
        for (uint32_t i = 1; i < 5; ++i)
        {
            out[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        }

        out[6] = 0;
        out[7] = 255;
    }
}

auto DecodeBlock(D3DFORMAT format, const BYTE* block, uint32_t out[16]) -> void
{
    const auto* color = [&]() -> const BYTE*
    {
        if (format == D3DFMT_DXT1)
        {
            return block;
        }

        return block + 8;
    }();

    const auto c0 = uint32_t{ Load16(color) };
    const auto c1 = uint32_t{ Load16(color + 2) };
    uint32_t   palette[4];
    ColorPalette(c0, c1, format == D3DFMT_DXT1 && c0 <= c1, palette);
    for (int i = 0; i < 16; ++i)
    {
        out[i] = palette[(color[4 + i / 4] >> ((i % 4) * 2)) & 3];
    }

    if (format == D3DFMT_DXT2 || format == D3DFMT_DXT3)
    {
        for (int i = 0; i < 16; ++i)
        {
            out[i] = (out[i] & 0x00FFFFFF) | (Widen((block[i / 2] >> ((i % 2) * 4)) & 0xF, 4) << 24);
        }
    }
    else if (format == D3DFMT_DXT4 || format == D3DFMT_DXT5)
    {
        uint32_t alphas[8];
        AlphaPalette(block[0], block[1], alphas);
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i)
        {
            bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
        }

        for (int i = 0; i < 16; ++i)
        {
            out[i] = (out[i] & 0x00FFFFFF) | (alphas[(bits >> (3 * i)) & 7] << 24);
        }
    }
}

constexpr auto Distance(uint32_t a, uint32_t b) -> uint32_t
{
    const auto dr = static_cast<int>(Red(a)) - static_cast<int>(Red(b));
    const auto dg = static_cast<int>(Green(a)) - static_cast<int>(Green(b));
    const auto db = static_cast<int>(Blue(a)) - static_cast<int>(Blue(b));
    return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

// Endpoints from the bounding box of the block's colours, each pixel taking the nearest palette entry.
auto EncodeColorBlock(const uint32_t pixels[16], bool dxt1, BYTE* out) -> void
{
    auto transparent = false;
    auto low         = uint32_t{ 0x00FFFFFF };
    auto high        = uint32_t{ 0 };
    for (int i = 0; i < 16; ++i)
    {
        if (dxt1 && Alpha(pixels[i]) < 0x80)
        {
            transparent = true;
            continue;
        }

        low  = Argb(0, std::min(Red(low), Red(pixels[i])), std::min(Green(low), Green(pixels[i])), std::min(Blue(low), Blue(pixels[i])));
        high = Argb(0, std::max(Red(high), Red(pixels[i])), std::max(Green(high), Green(pixels[i])), std::max(Blue(high), Blue(pixels[i])));
    }

    auto c0 = To565(high);
    auto c1 = To565(low);
    // Four colours need c0 > c1, the transparent three-colour mode c0 <= c1.
    const auto outOfOrder = [&]() -> bool
    {
        if (transparent)
        {
            return c0 > c1;
        }

        return c0 < c1;
    }();

    if (outOfOrder)
    {
        std::swap(c0, c1);
    }

    uint32_t palette[4];
    ColorPalette(c0, c1, transparent || c0 == c1, palette);

    auto indices = uint32_t{ 0 };
    for (int i = 0; i < 16; ++i)
    {
        auto best = uint32_t{ 0 };
        if (transparent && Alpha(pixels[i]) < 0x80)
        {
            best = 3;
        }
        else
        {
            const auto choices = [&]() -> uint32_t
            {
                if (transparent || c0 == c1)
                {
                    return 3;
                }

                return 4;
            }();

            for (uint32_t j = 1; j < choices; ++j)
            {
                if (Distance(pixels[i], palette[j]) < Distance(pixels[i], palette[best]))
                {
                    best = j;
                }
            }
        }

        indices |= best << (2 * i);
    }

    Store16(out, c0);
    Store16(out + 2, c1);
    for (int i = 0; i < 4; ++i)
    {
        out[4 + i] = static_cast<BYTE>(indices >> (8 * i));
    }
}

auto EncodeAlphaBlock(const uint32_t pixels[16], bool interpolated, BYTE* out) -> void
{
    if (!interpolated)
    {
        for (int i = 0; i < 8; ++i)
        {
            out[i] = static_cast<BYTE>(Narrow(Alpha(pixels[2 * i]), 4) | (Narrow(Alpha(pixels[2 * i + 1]), 4) << 4));
        }

        return;
    }

    auto a0 = uint32_t{ 0 };
    auto a1 = uint32_t{ 255 };
    for (int i = 0; i < 16; ++i)
    {
        a0 = std::max(a0, Alpha(pixels[i]));
        a1 = std::min(a1, Alpha(pixels[i]));
    }

    uint32_t alphas[8];
    AlphaPalette(a0, a1, alphas);
    auto bits = uint64_t{ 0 };
    for (int i = 0; i < 16; ++i)
    {
        auto best = uint32_t{ 0 };
        for (uint32_t j = 1; j < 8; ++j)
        {
            const auto dj = static_cast<int>(alphas[j]) - static_cast<int>(Alpha(pixels[i]));
            const auto db = static_cast<int>(alphas[best]) - static_cast<int>(Alpha(pixels[i]));
            if (dj * dj < db * db)
            {
                best = j;
            }
        }

        bits |= static_cast<uint64_t>(best) << (3 * i);
    }

    out[0] = static_cast<BYTE>(a0);
    out[1] = static_cast<BYTE>(a1);
    for (int i = 0; i < 6; ++i)
    {
        out[2 + i] = static_cast<BYTE>(bits >> (8 * i));
    }
}

} // namespace

auto Decode(D3DFORMAT format, const BYTE* bits, UINT pitch, const PALETTEENTRY* palette, const RECT& rect, Image& image) -> bool
{
    image.width  = static_cast<UINT>(rect.right - rect.left);
    image.height = static_cast<UINT>(rect.bottom - rect.top);
    image.pixels.assign(static_cast<size_t>(image.width) * image.height, 0);
    const auto layout = LayoutOf(format);
    if (layout.blockEdge == 4)
    {
        for (LONG by = rect.top / 4; by * 4 < rect.bottom; ++by)
        {
            for (LONG bx = rect.left / 4; bx * 4 < rect.right; ++bx)
            {
                uint32_t block[16];
                DecodeBlock(format, bits + by * pitch + bx * layout.blockBytes, block);
                for (int i = 0; i < 16; ++i)
                {
                    const auto x = bx * 4 + i % 4;
                    const auto y = by * 4 + i / 4;
                    if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                    {
                        image.pixels[(y - rect.top) * image.width + (x - rect.left)] = block[i];
                    }
                }
            }
        }

        return true;
    }

    const auto pixelBytes = layout.blockBytes;
    if (pixelBytes == 0)
    {
        return false;
    }

    for (UINT y = 0; y < image.height; ++y)
    {
        const auto* row = bits + (rect.top + y) * pitch + rect.left * pixelBytes;
        for (UINT x = 0; x < image.width; ++x)
        {
            const auto argb = ReadPixel(format, row + x * pixelBytes, palette);
            if (!argb)
            {
                return false;
            }

            image.pixels[y * image.width + x] = *argb;
        }
    }

    return true;
}

auto Encode(D3DFORMAT format, BYTE* bits, UINT pitch, const Image& image) -> bool
{
    const auto layout = LayoutOf(format);
    if (layout.blockEdge == 4)
    {
        for (UINT by = 0; by * 4 < image.height; ++by)
        {
            for (UINT bx = 0; bx * 4 < image.width; ++bx)
            {
                // Edge blocks repeat the last row and column.
                uint32_t block[16];
                for (UINT i = 0; i < 16; ++i)
                {
                    const auto x = std::min(bx * 4 + i % 4, image.width - 1);
                    const auto y = std::min(by * 4 + i / 4, image.height - 1);
                    block[i]     = image.pixels[y * image.width + x];
                }

                auto* const out = bits + by * pitch + bx * layout.blockBytes;
                if (format == D3DFMT_DXT1)
                {
                    EncodeColorBlock(block, true, out);
                }
                else
                {
                    EncodeAlphaBlock(block, format == D3DFMT_DXT4 || format == D3DFMT_DXT5, out);
                    EncodeColorBlock(block, false, out + 8);
                }
            }
        }

        return true;
    }

    const auto pixelBytes = layout.blockBytes;
    if (pixelBytes == 0)
    {
        return false;
    }

    for (UINT y = 0; y < image.height; ++y)
    {
        for (UINT x = 0; x < image.width; ++x)
        {
            if (!WritePixel(format, image.pixels[y * image.width + x], bits + y * pitch + x * pixelBytes))
            {
                return false;
            }
        }
    }

    return true;
}

auto Resample(const Image& source, UINT width, UINT height, DWORD filter) -> Image
{
    if (source.width == width && source.height == height)
    {
        return source;
    }

    auto out   = Image{};
    out.width  = width;
    out.height = height;
    out.pixels.assign(static_cast<size_t>(width) * height, 0);
    const auto kind = filter & 0xFF;
    for (UINT y = 0; y < height; ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            auto& pixel = out.pixels[y * width + x];
            if (kind == D3DX_FILTER_NONE)
            {
                if (x < source.width && y < source.height)
                {
                    pixel = source.pixels[y * source.width + x];
                }

                continue;
            }

            const auto x0 = x * source.width / width;
            const auto y0 = y * source.height / height;
            if (kind == D3DX_FILTER_POINT)
            {
                pixel = source.pixels[y0 * source.width + x0];
                continue;
            }

            const auto x1     = std::max(x0 + 1, (x + 1) * source.width / width);
            const auto y1     = std::max(y0 + 1, (y + 1) * source.height / height);
            uint32_t   sum[4] = {};
            for (UINT sy = y0; sy < y1; ++sy)
            {
                for (UINT sx = x0; sx < x1; ++sx)
                {
                    const auto p = source.pixels[sy * source.width + sx];
                    sum[0] += Alpha(p);
                    sum[1] += Red(p);
                    sum[2] += Green(p);
                    sum[3] += Blue(p);
                }
            }

            const auto count = (x1 - x0) * (y1 - y0);
            pixel            = Argb((sum[0] + count / 2) / count, (sum[1] + count / 2) / count, (sum[2] + count / 2) / count, (sum[3] + count / 2) / count);
        }
    }

    return out;
}

} // namespace nulldx
