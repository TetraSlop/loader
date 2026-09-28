#pragma once
#include <windows.h>

#include "texture_pack.h"
#include <atomic>
#include <wrl/client.h>

namespace tmloader::detail
{

// D3DSPD_IUNKNOWN owns the category and HD texture as a single attachment.
class Replacement final : public IUnknown
{
public:
    Replacement(IUnknown* texture, TextureCategory category) noexcept;
    ~Replacement()                                     = default;
    Replacement(const Replacement&)                    = delete;
    auto operator=(const Replacement&) -> Replacement& = delete;
    Replacement(Replacement&&)                         = delete;
    auto operator=(Replacement&&) -> Replacement&      = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override;
    ULONG STDMETHODCALLTYPE   AddRef() override;
    ULONG STDMETHODCALLTYPE   Release() override;
    auto                      texture() const noexcept -> IUnknown*;
    auto                      enabled(bool artwork, bool fonts) const noexcept -> bool;

private:
    std::atomic<ULONG>               references_{ 1 };
    Microsoft::WRL::ComPtr<IUnknown> texture_;
    const TextureCategory            category_;
};

} // namespace tmloader::detail
