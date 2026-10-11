#pragma once
#include <windows.h>

#include "textures/texture_pack.h"
#include <wrl/client.h>
#include <wrl/implements.h>

namespace tmloader::detail
{

// D3DSPD_IUNKNOWN owns the category and HD texture as a single attachment.
class Replacement final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUnknown>
{
public:
    Replacement(IUnknown* texture, TextureCategory category) noexcept;
    auto texture() const noexcept -> IUnknown*;
    auto enabled(bool artwork, bool fonts) const noexcept -> bool;

private:
    Microsoft::WRL::ComPtr<IUnknown> texture_;
    const TextureCategory            category_;
};

} // namespace tmloader::detail
