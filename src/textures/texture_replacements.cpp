#include "textures/texture_replacements.h"
#include "app/runtime.h"
#include "textures/texture_pack.h"
#include "textures/texture_replacement.h"
#include <atomic>
#include <new>
#include <wrl/client.h>

namespace tmloader
{
namespace
{

TexturePack           pack;
TextureUploadFn       nativeUpload = nullptr;
std::atomic<size_t>   packCount{ 0 };
std::atomic<unsigned> loadedCount{ 0 };
// The original resource owns its replacement through D3DSPD_IUNKNOWN.
constexpr GUID replacementKey{ 0x75b28fd7, 0x62b1, 0x48a8, { 0x93, 0x28, 0x4e, 0x98, 0xa8, 0xca, 0x47, 0x6d } };
using detail::Replacement;

template <class... Args>
auto call(void* object, int slot, Args... args) -> HRESULT
{
    using Method = HRESULT(WINAPI*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(object, args...);
}

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

void initializeTextureReplacements(BYTE* game, const std::filesystem::path& manifest)
{
    try
    {
        if (!std::filesystem::exists(manifest))
        {
            logf("HD artwork: no texture pack installed");
            return;
        }
        pack.load(manifest);
        if (!pack.size())
        {
            return;
        }
        nativeUpload = installTextureCallHooks(game, uploadTextureWithReplacement);
        packCount    = pack.size();
        logf("HD artwork: indexed %zu source images from %ls", pack.size(), manifest.c_str());
    }
    catch (const std::exception& e)
    {
        logf("HD artwork disabled; originals retained: %s", e.what());
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
    using SetPrivateData = HRESULT(WINAPI*)(void*, REFGUID, const void*, DWORD, DWORD);
    const auto    set    = reinterpret_cast<SetPrivateData>((*static_cast<void***>(original))[4]);
    const HRESULT hr     = set(original, replacementKey, attachment, sizeof(IUnknown*), 1);
    attachment->Release();
    if (SUCCEEDED(hr))
    {
        ++loadedCount;
    }
    return hr;
}

auto WINAPI uploadTextureWithReplacement(void* device, const void* source, UINT bytes, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey, void* info, void* palette, void** output) -> HRESULT
{
    // Always create and return the original resource. Native GetLevelDesc,
    // UV calculations, font metadata, reference counts and reset code stay valid.
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
        DWORD desc[8]{};
        if (FAILED(call(*output, 14, 0U, desc)) || desc[6] < asset->width || desc[7] < asset->height ||
            desc[6] > 8192 / asset->scale || desc[7] > 8192 / asset->scale)
        {
            return originalResult;
        }
        const auto                       replacementPng = pack.read(*asset);
        Microsoft::WRL::ComPtr<IUnknown> replacement;
        // FILTER_NONE keeps the PNG at the top left and pads the remainder.
        // Scale the original GPU footprint, including POT padding, not just IHDR.
        HRESULT hr = nativeUpload(device, replacementPng.data(), static_cast<UINT>(replacementPng.size()), desc[6] * asset->scale, desc[7] * asset->scale, 1, 0, desc[0], 1 /* MANAGED */, filter, mipFilter, 0, nullptr, nullptr, reinterpret_cast<void**>(replacement.GetAddressOf()));
        DWORD   actual[8]{};
        if (SUCCEEDED(hr) && replacement)
        {
            hr = call(replacement.Get(), 14, 0U, actual);
        }
        if (SUCCEEDED(hr) && (actual[6] != desc[6] * asset->scale || actual[7] != desc[7] * asset->scale))
        {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr))
        {
            hr = attachTextureReplacement(*output, replacement.Get(), asset->category);
        }
        if (SUCCEEDED(hr))
        {
            logf("HD artwork: %s logical=%lux%lu GPU=%lux%lu", asset->id.c_str(), desc[6], desc[7], actual[6], actual[7]);
        }
        else
        {
            logf("HD artwork fallback: %s upload=%08lX", asset->id.c_str(), hr);
        }
    }
    catch (const std::exception& e)
    {
        logf("HD artwork fallback: %s", e.what());
    }
    return originalResult;
}

TextureDrawBinding::TextureDrawBinding(void* device, bool enabled)
: TextureDrawBinding(device, enabled, enabled)
{
}

TextureDrawBinding::TextureDrawBinding(void* device, bool artworkEnabled, bool fontsEnabled)
: device_(device)
{
    if ((!artworkEnabled && !fontsEnabled) || !loadedCount)
    {
        return;
    }
    if (FAILED(call(device_, 60, 0U, &original_)) || !original_)
    {
        return;
    }
    DWORD size           = sizeof(IUnknown*);
    using GetPrivateData = HRESULT(WINAPI*)(void*, REFGUID, void*, DWORD*);
    const auto get       = reinterpret_cast<GetPrivateData>((*static_cast<void***>(original_))[5]);
    if (FAILED(get(original_, replacementKey, &attachment_, &size)) || !attachment_)
    {
        return;
    }
    auto* const replacement = static_cast<Replacement*>(attachment_);
    if (replacement->enabled(artworkEnabled, fontsEnabled))
    {
        bound_ = SUCCEEDED(call(device_, 61, 0U, replacement->texture()));
    }
}

TextureDrawBinding::~TextureDrawBinding()
{
    if (bound_ && FAILED(call(device_, 61, 0U, original_)))
    {
        logf("HD artwork: could not restore native texture binding");
    }
    release(attachment_);
    release(original_);
}

} // namespace tmloader
