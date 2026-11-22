// D3DXCreateTextureFromFileInMemoryEx on the host. TM.dll links D3DX 8 statically, and the game decodes PNG and
// JPEG files into new textures several times a frame: under the interpreter, libpng and D3DX's per-pixel x87
// conversion were most of a frame's instructions. This stand-in decodes with stb_image and fills the texture
// directly. It covers what the game asks for: one level, A8R8G8B8 or X8R8G8B8, no scaling filter, no palette. Any
// other call goes back to the game's own D3DX through a trampoline, so behaviour never depends on the stand-in's
// coverage.
#include "com/d3dx.h"

#include "com/proxies.h"
#include "core/machine.h"

#include <d3d8.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace tmweb
{
namespace
{

// TM.dll's copy (image base 0x10000000), and the bytes its first instructions must have before it is patched:
// push ebp; mov ebp, esp; push dword [ebp+40h]; push 3
constexpr std::uint32_t kCreateTextureFromFileInMemoryEx = 0x101B9AA4;
constexpr auto          kPrologue                        = std::array<std::uint8_t, 8>{ 0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x40, 0x6A, 0x03 };
// The trampoline replays the first three whole instructions, then jumps back past them
constexpr std::uint32_t kReplayed = 6;

constexpr auto kModule = "tmweb.d3dx";
constexpr auto kName   = "D3DXCreateTextureFromFileInMemoryEx";

enum : std::uint32_t
{
    kD3dxDefault    = 0xFFFFFFFF,
    kFilterMask     = 0xFF,
    kFilterNone     = 1,
    kTextureType    = 3, // D3DRTYPE_TEXTURE
    kFileBmp        = 0, // D3DXIMAGE_FILEFORMAT
    kFileJpg        = 1,
    kFileTga        = 2,
    kFilePng        = 3,
    kFormatL8       = 50,
    kFormatA8L8     = 51,
    kFormatA8R8G8B8 = 21,
    kFormatX8R8G8B8 = 22,
};

// Arguments, in order
enum Arg : unsigned
{
    kDevice,
    kSrcData,
    kSrcDataSize,
    kWidth,
    kHeight,
    kMipLevels,
    kUsage,
    kFormat,
    kPool,
    kFilter,
    kMipFilter,
    kColorKey,
    kSrcInfo,
    kPalette,
    kTexture,
};

struct State
{
    bool          patched    = false;
    std::uint32_t trampoline = 0;
};

// Verification tallies, by file format and channel count
struct Tally
{
    std::uint32_t same      = 0;
    std::uint32_t different = 0;
};

auto g_verify  = false;
auto g_tallies = std::map<std::pair<std::uint32_t, int>, Tally>{};

// Run the game's own D3DX for the same call and compare its texels with ours
void verify(Call& call, Proxies& proxies, const State& state, IDirect3DTexture8* ours, const std::uint32_t fileKind, const int channels)
{
    auto&      memory = call.memory();
    const auto out    = memory.allocate(4);
    auto       args   = std::array<std::uint32_t, 15>{};
    for (auto index = 0u; index < args.size(); ++index)
    {
        args[index] = call.arg(index);
    }
    args[kTexture]     = out;
    const auto result  = call.machine().callGuest(state.trampoline, args, Convention::Stdcall);
    auto*      theirs  = proxies.unwrap<IDirect3DTexture8>(memory.read<std::uint32_t>(out));
    auto       same    = SUCCEEDED(static_cast<HRESULT>(result)) && theirs != nullptr;
    auto       ourDesc = D3DSURFACE_DESC{};
    auto       herDesc = D3DSURFACE_DESC{};
    if (same)
    {
        ours->GetLevelDesc(0, &ourDesc);
        theirs->GetLevelDesc(0, &herDesc);
        same = ourDesc.Width == herDesc.Width && ourDesc.Height == herDesc.Height && ourDesc.Format == herDesc.Format;
    }
    if (same)
    {
        auto a = D3DLOCKED_RECT{};
        auto b = D3DLOCKED_RECT{};
        ours->LockRect(0, &a, nullptr, D3DLOCK_READONLY);
        theirs->LockRect(0, &b, nullptr, D3DLOCK_READONLY);
        for (auto y = 0u; y < ourDesc.Height && same; ++y)
        {
            same = std::memcmp(static_cast<const std::uint8_t*>(a.pBits) + static_cast<std::size_t>(y) * a.Pitch, static_cast<const std::uint8_t*>(b.pBits) + static_cast<std::size_t>(y) * b.Pitch, static_cast<std::size_t>(ourDesc.Width) * 4) == 0;
        }
        ours->UnlockRect(0);
        theirs->UnlockRect(0);
    }
    if (theirs != nullptr)
    {
        theirs->Release();
    }
    memory.free(out);
    if (!same && g_tallies[{ fileKind, channels }].different < 3)
    {
        std::fprintf(stderr, "d3dx: ours %ux%u fmt %u, theirs %ux%u fmt %u, result %#x\n", ourDesc.Width, ourDesc.Height, ourDesc.Format, herDesc.Width, herDesc.Height, herDesc.Format, result);
        if (ourDesc.Width == herDesc.Width && ourDesc.Height == herDesc.Height && theirs != nullptr)
        {
            auto a = D3DLOCKED_RECT{};
            auto b = D3DLOCKED_RECT{};
            ours->LockRect(0, &a, nullptr, D3DLOCK_READONLY);
            theirs->LockRect(0, &b, nullptr, D3DLOCK_READONLY);
            auto shown = 0;
            for (auto y = 0u; y < ourDesc.Height && shown < 6; ++y)
            {
                for (auto x = 0u; x < ourDesc.Width && shown < 6; ++x)
                {
                    std::uint32_t p = 0;
                    std::uint32_t q = 0;
                    std::memcpy(&p, static_cast<const std::uint8_t*>(a.pBits) + y * a.Pitch + x * 4, 4);
                    std::memcpy(&q, static_cast<const std::uint8_t*>(b.pBits) + y * b.Pitch + x * 4, 4);
                    if (p != q)
                    {
                        std::fprintf(stderr, "  (%u,%u) ours %08x theirs %08x\n", x, y, p, q);
                        ++shown;
                    }
                }
            }
            ours->UnlockRect(0);
            theirs->UnlockRect(0);
        }
    }
    auto& tally = g_tallies[{ fileKind, channels }];
    if (same)
    {
        ++tally.same;
    }
    else
    {
        ++tally.different;
    }
}

auto roundUpPow2(const std::uint32_t value) -> std::uint32_t
{
    auto result = 1u;
    while (result < value)
    {
        result <<= 1;
    }
    return result;
}

auto fileFormat(const std::vector<std::uint8_t>& bytes) -> std::uint32_t
{
    if (bytes.size() >= 4 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
    {
        return kFilePng;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8)
    {
        return kFileJpg;
    }
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
    {
        return kFileBmp;
    }
    return kFileTga;
}

// The source format D3DXIMAGE_INFO reports, from the decoded channel count
auto sourceFormat(const int channels) -> std::uint32_t
{
    switch (channels)
    {
        case 1:
            return kFormatL8;
        case 2:
            return kFormatA8L8;
        case 3:
            return kFormatX8R8G8B8;
        default:
            return kFormatA8R8G8B8;
    }
}

// Patch the game's D3DX entry to jump to the stand-in, and build the trampoline that runs the original
void patch(Machine& machine, State& state)
{
    state.patched = true;
    auto& memory  = machine.memory();
    if (!memory.mapped(kCreateTextureFromFileInMemoryEx, static_cast<std::uint32_t>(kPrologue.size())))
    {
        return;
    }
    auto code = std::array<std::uint8_t, kPrologue.size()>{};
    memory.read(kCreateTextureFromFileInMemoryEx, code);
    if (code != kPrologue)
    {
        return; // not the pinned TM.dll: leave its D3DX alone
    }
    state.trampoline = memory.allocate(16);
    memory.write(state.trampoline, std::span{ kPrologue.data(), kReplayed });
    memory.write<std::uint8_t>(state.trampoline + kReplayed, 0xE9);
    memory.write<std::uint32_t>(state.trampoline + kReplayed + 1, kCreateTextureFromFileInMemoryEx + kReplayed - (state.trampoline + kReplayed + 5));
    const auto entry = machine.boundary().resolve(kModule, kName);
    memory.write<std::uint8_t>(kCreateTextureFromFileInMemoryEx, 0xE9);
    memory.write<std::uint32_t>(kCreateTextureFromFileInMemoryEx + 1, entry - (kCreateTextureFromFileInMemoryEx + 5));
}

// The game's own D3DX, from the trampoline: the stack is as the caller left it
auto fallback(Call& call, const State& state) -> Outcome
{
    call.context().eip = state.trampoline;
    return Resume{};
}

auto createTexture(Call& call, Proxies& proxies, const State& state) -> Outcome
{
    auto& memory = call.memory();
    if (call.arg(kPalette) != 0 || call.arg(kMipLevels) != 1 || (call.arg(kFilter) & kFilterMask) != kFilterNone)
    {
        return fallback(call, state);
    }
    const auto format = call.arg(kFormat);
    if (format != kFormatA8R8G8B8 && format != kFormatX8R8G8B8)
    {
        return fallback(call, state);
    }
    auto* const device = proxies.unwrap<IDirect3DDevice8>(call.arg(kDevice));
    if (device == nullptr || call.arg(kSrcData) == 0 || call.arg(kSrcDataSize) == 0)
    {
        return fallback(call, state);
    }
    auto bytes = std::vector<std::uint8_t>(call.arg(kSrcDataSize));
    memory.read(call.arg(kSrcData), bytes);
    auto width    = 0;
    auto height   = 0;
    auto channels = 0;
    auto pixels   = std::unique_ptr<stbi_uc, void (*)(void*)>{ stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4), stbi_image_free };
    if (!pixels)
    {
        return fallback(call, state);
    }
    // Only what was checked byte for byte against the game's D3DX (--verify-d3dx): RGBA PNGs. Everything else, JPEG
    // with its IDCT rounding included, stays with the game's own code.
    if (fileFormat(bytes) != kFilePng || channels != 4)
    {
        return fallback(call, state);
    }

    // The texture's size: as asked, or the image's, rounded as the device's caps require, as D3DX does
    auto caps = D3DCAPS8{};
    device->GetDeviceCaps(&caps);
    auto textureWidth  = call.arg(kWidth);
    auto textureHeight = call.arg(kHeight);
    if (textureWidth == kD3dxDefault || textureWidth == 0)
    {
        textureWidth = static_cast<std::uint32_t>(width);
    }
    if (textureHeight == kD3dxDefault || textureHeight == 0)
    {
        textureHeight = static_cast<std::uint32_t>(height);
    }
    if (caps.TextureCaps & D3DPTEXTURECAPS_POW2)
    {
        textureWidth  = roundUpPow2(textureWidth);
        textureHeight = roundUpPow2(textureHeight);
    }
    if (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY)
    {
        textureWidth  = std::max(textureWidth, textureHeight);
        textureHeight = textureWidth;
    }
    // With no filter the image is copied unscaled; a smaller texture would need cropping, which D3DX does not do
    if (textureWidth < static_cast<std::uint32_t>(width) || textureHeight < static_cast<std::uint32_t>(height))
    {
        return fallback(call, state);
    }

    auto*      texture = static_cast<IDirect3DTexture8*>(nullptr);
    const auto created = device->CreateTexture(textureWidth, textureHeight, 1, call.arg(kUsage), static_cast<D3DFORMAT>(format), static_cast<D3DPOOL>(call.arg(kPool)), &texture);
    if (FAILED(created) || texture == nullptr)
    {
        return Return{ static_cast<std::uint32_t>(created) };
    }
    auto locked = D3DLOCKED_RECT{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
    {
        texture->Release();
        return fallback(call, state);
    }
    // Pixels outside the image are transparent black, as with D3DX_FILTER_NONE
    const auto colorKey = call.arg(kColorKey);
    for (auto y = 0u; y < textureHeight; ++y)
    {
        auto* const row = static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(y) * static_cast<std::size_t>(locked.Pitch);
        std::memset(row, 0, static_cast<std::size_t>(textureWidth) * 4);
        if (y >= static_cast<std::uint32_t>(height))
        {
            continue;
        }
        const auto* source = pixels.get() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (auto x = 0; x < width; ++x, source += 4)
        {
            auto alpha = static_cast<std::uint32_t>(source[3]);
            if (format == kFormatX8R8G8B8)
            {
                alpha = 0xFF;
            }
            const auto argb = (alpha << 24) | (static_cast<std::uint32_t>(source[0]) << 16) | (static_cast<std::uint32_t>(source[1]) << 8) | source[2];
            if (colorKey != 0 && argb == colorKey)
            {
                continue;
            }
            std::memcpy(row + static_cast<std::size_t>(x) * 4, &argb, 4);
        }
    }
    texture->UnlockRect(0);
    if (g_verify)
    {
        verify(call, proxies, state, texture, fileFormat(bytes), channels);
    }

    if (const auto info = call.arg(kSrcInfo))
    {
        const auto fields = std::array<std::uint32_t, 7>{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1, 1, sourceFormat(channels), kTextureType, fileFormat(bytes) };
        for (auto index = 0u; index < fields.size(); ++index)
        {
            memory.write<std::uint32_t>(info + 4 * index, fields[index]);
        }
    }
    memory.write<std::uint32_t>(call.arg(kTexture), proxies.wrap<IDirect3DTexture8>(texture));
    return Return{ D3D_OK };
}

} // namespace

void verifyD3dx(const bool on)
{
    g_verify = on;
}

auto d3dxReport() -> std::string
{
    auto report = std::string{};
    for (const auto& [key, tally] : g_tallies)
    {
        static constexpr const char* kKinds[] = { "BMP", "JPEG", "TGA", "PNG" };
        report += std::format("{} with {} channels: {} identical, {} different\n", kKinds[std::min(key.first, 3u)], key.second, tally.same, tally.different);
    }
    return report;
}

void addD3dx(Proxies& proxies, std::vector<Function>& functions)
{
    auto state = std::make_shared<State>();
    functions.push_back(Function{ kModule, kName, Convention::Stdcall, 15, [&proxies, state](Call& call) -> Outcome
                                  {
                                      return createTexture(call, proxies, *state);
                                  } });
    for (auto& function : functions)
    {
        if (function.module == "d3d8.dll" && function.name == "Direct3DCreate8")
        {
            function.handler = [state, original = std::move(function.handler)](Call& call) -> Outcome
            {
                if (!state->patched)
                {
                    patch(call.machine(), *state);
                }
                return original(call);
            };
        }
    }
}

} // namespace tmweb
