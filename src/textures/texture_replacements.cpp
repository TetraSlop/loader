#include "textures/texture_replacements.h"
#include "app/log.h"
#include "graphics/d3d8_abi.h"
#include "platform/patch.h"
#include "textures/texture_pack.h"
#include "textures/texture_replacement.h"
#include <atomic>
#include <new>
#include <wrl/client.h>

namespace tmloader
{
namespace
{

// Private-data key under which each original texture owns its replacement.
constexpr GUID kReplacementKey{ 0x75b28fd7, 0x62b1, 0x48a8, { 0x93, 0x28, 0x4e, 0x98, 0xa8, 0xca, 0x47, 0x6d } };

// Every call the game makes to its texture loader.
constexpr DWORD kUploadRva     = 0x1b9aa4;
constexpr DWORD kUploadCalls[] = { 0x17ba39, 0x17c061, 0x17c1f5, 0x17c393, 0x17c528, 0x17ccfa, 0x17ce91 };

TexturePack           pack;
TextureUploadFn       nativeUpload = nullptr;
std::atomic<size_t>   packCount{ 0 };
std::atomic<unsigned> loadedCount{ 0 };

using detail::Replacement;

void release(void* object)
{
    if (object)
    {
        static_cast<IUnknown*>(object)->Release();
    }
}

} // namespace

namespace detail
{

Replacement::Replacement(IUnknown* texture, TextureCategory category) noexcept
: texture_(texture)
, category_(category)
{
}

auto STDMETHODCALLTYPE Replacement::QueryInterface(REFIID iid, void** output) -> HRESULT
{
    if (!output)
    {
        return E_POINTER;
    }

    *output = nullptr;
    if (iid != __uuidof(IUnknown))
    {
        return E_NOINTERFACE;
    }

    *output = this;
    AddRef();
    return S_OK;
}

auto STDMETHODCALLTYPE Replacement::AddRef() -> ULONG
{
    return ++references_;
}

auto STDMETHODCALLTYPE Replacement::Release() -> ULONG
{
    const auto remaining = --references_;
    if (!remaining)
    {
        delete this;
    }

    return remaining;
}

auto Replacement::texture() const noexcept -> IUnknown*
{
    return texture_.Get();
}

auto Replacement::enabled(bool artwork, bool fonts) const noexcept -> bool
{
    if (category_ == TextureCategory::font)
    {
        return fonts;
    }

    return artwork;
}

} // namespace detail

auto texturePackCount() -> size_t
{
    return packCount;
}

auto textureReplacementCount() -> unsigned
{
    return loadedCount;
}

void configureTextureReplacements(const std::filesystem::path& manifest, TextureUploadFn upload)
{
    pack.load(manifest);
    nativeUpload = upload;
    packCount    = pack.size();
    loadedCount  = 0;
}

void initializeTextureReplacements(HMODULE game, const std::filesystem::path& manifest)
{
    try
    {
        if (!std::filesystem::exists(manifest))
        {
            logLine("Texture pack: none installed");
            return;
        }

        pack.load(manifest);
        if (!pack.size())
        {
            return;
        }

        for (const auto call : kUploadCalls)
        {
            nativeUpload = reinterpret_cast<TextureUploadFn>(redirectCall(game, call, kUploadRva, reinterpret_cast<void*>(uploadTextureWithReplacement)));
        }

        packCount = pack.size();
        logLine("Texture pack: {} images from {}", pack.size(), narrow(manifest.native()));
    }
    catch (const std::exception& e)
    {
        logLine("Texture pack not loaded: {}", e.what());
    }
}

auto attachTextureReplacement(void* original, void* replacement, TextureCategory category) -> HRESULT
{
    if (!original || !replacement || original == replacement ||
        (category != TextureCategory::artwork && category != TextureCategory::font))
    {
        return E_INVALIDARG;
    }

    auto* const attachment = new (std::nothrow) Replacement(static_cast<IUnknown*>(replacement), category);
    if (!attachment)
    {
        return E_OUTOFMEMORY;
    }

    const auto hr = callMethod(original, d3d8::SetPrivateData, &kReplacementKey, static_cast<const void*>(attachment), DWORD{ sizeof(IUnknown*) }, d3d8::kPrivateDataObject);
    attachment->Release();
    if (SUCCEEDED(hr))
    {
        ++loadedCount;
    }

    return hr;
}

auto WINAPI uploadTextureWithReplacement(void* device, const void* source, UINT bytes, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey, void* info, void* palette, void** output) -> HRESULT
{
    // The game always gets its original texture; the replacement is only bound while drawing.
    if (!nativeUpload)
    {
        return E_UNEXPECTED;
    }

    const HRESULT originalResult = nativeUpload(device, source, bytes, width, height, levels, usage, format, pool, filter, mipFilter, colorKey, info, palette, output);
    if (FAILED(originalResult) || !output || !*output)
    {
        return originalResult;
    }

    try
    {
        const auto* const asset = pack.match(source, bytes);
        if (!asset || levels != 1 || usage || colorKey || (filter & 0xffff) != 1)
        {
            return originalResult;
        }

        auto desc = d3d8::SurfaceDesc{};
        if (FAILED(callMethod(*output, d3d8::GetLevelDesc, 0U, &desc)) || desc.width < asset->width || desc.height < asset->height ||
            desc.width > 8192 / asset->scale || desc.height > 8192 / asset->scale)
        {
            return originalResult;
        }

        const auto                       replacementPng = pack.read(*asset);
        Microsoft::WRL::ComPtr<IUnknown> replacement;
        // Scale the padded power-of-two footprint so the game's UVs stay valid.
        auto hr     = nativeUpload(device, replacementPng.data(), static_cast<UINT>(replacementPng.size()), desc.width * asset->scale, desc.height * asset->scale, 1, 0, desc.format, d3d8::kPoolManaged, filter, mipFilter, 0, nullptr, nullptr, reinterpret_cast<void**>(replacement.GetAddressOf()));
        auto actual = d3d8::SurfaceDesc{};
        if (SUCCEEDED(hr) && replacement)
        {
            hr = callMethod(replacement.Get(), d3d8::GetLevelDesc, 0U, &actual);
        }

        if (SUCCEEDED(hr) && (actual.width != desc.width * asset->scale || actual.height != desc.height * asset->scale))
        {
            hr = E_FAIL;
        }

        if (SUCCEEDED(hr))
        {
            hr = attachTextureReplacement(*output, replacement.Get(), asset->category);
        }

        if (SUCCEEDED(hr))
        {
            logLine("Texture pack: {} {}x{} -> {}x{}", asset->id, desc.width, desc.height, actual.width, actual.height);
        }
        else
        {
            logLine("Texture pack: {} kept its original ({:08X})", asset->id, hex(hr));
        }
    }
    catch (const std::exception& e)
    {
        logLine("Texture pack: {}", e.what());
    }

    return originalResult;
}

TextureDrawBinding::TextureDrawBinding(void* device, bool artworkEnabled, bool fontsEnabled)
: device_(device)
{
    if ((!artworkEnabled && !fontsEnabled) || !loadedCount)
    {
        return;
    }

    if (FAILED(callMethod(device_, d3d8::GetTexture, 0U, &original_)) || !original_)
    {
        return;
    }

    auto size = DWORD{ sizeof(IUnknown*) };
    if (FAILED(callMethod(original_, d3d8::GetPrivateData, &kReplacementKey, static_cast<void*>(&attachment_), &size)) || !attachment_)
    {
        return;
    }

    auto* const replacement = static_cast<Replacement*>(attachment_);
    if (replacement->enabled(artworkEnabled, fontsEnabled))
    {
        bound_ = SUCCEEDED(callMethod(device_, d3d8::SetTexture, 0U, replacement->texture()));
    }
}

TextureDrawBinding::~TextureDrawBinding()
{
    if (bound_ && FAILED(callMethod(device_, d3d8::SetTexture, 0U, original_)))
    {
        logLine("Texture pack: cannot restore the original texture binding");
    }

    release(attachment_);
    release(original_);
}

} // namespace tmloader
