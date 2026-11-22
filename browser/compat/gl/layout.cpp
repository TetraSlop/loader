#include "internal.h"

namespace gl
{

auto TexCoordSize(DWORD fvf, uint32_t set) noexcept -> uint32_t
{
    constexpr auto kSizes = std::array<uint32_t, 4>{ 2, 3, 4, 1 };
    return kSizes[(fvf >> (16 + set * 2)) & 3];
}

auto FvfLayout(DWORD fvf) -> std::vector<Element>
{
    auto       layout = std::vector<Element>{};
    auto       offset = uint32_t{ 0 };
    const auto add    = [&](Attribute attribute, uint32_t components, bool color = false)
    {
        layout.push_back({ attribute, 0, offset, components, color });
        if (color)
        {
            offset += 4;
        }
        else
        {
            offset += components * 4;
        }
    };

    const auto position = fvf & D3DFVF_POSITION_MASK;
    if (position == D3DFVF_XYZRHW)
    {
        add(kPosition, 4);
    }
    else if (position != 0)
    {
        add(kPosition, 3);
        // XYZB1-XYZB5 carry 1-5 betas; with LASTBETA_UBYTE4 the last are blend indices, which nothing reads.
        const auto betas = [&]() -> uint32_t
        {
            if (position >= D3DFVF_XYZB1)
            {
                return (position - D3DFVF_XYZRHW) / 2;
            }

            return 0;
        }();

        const auto weights = [&]() -> uint32_t
        {
            if ((fvf & D3DFVF_LASTBETA_UBYTE4) != 0 && betas > 0)
            {
                return betas - 1;
            }

            return betas;
        }();

        if (weights > 0)
        {
            add(kBlendWeights, std::min(weights, 4u));
        }

        offset += (betas - std::min(weights, 4u)) * 4;
    }

    if ((fvf & D3DFVF_NORMAL) != 0)
    {
        add(kNormal, 3);
    }

    if ((fvf & D3DFVF_PSIZE) != 0)
    {
        add(kPointSize, 1);
    }

    if ((fvf & D3DFVF_DIFFUSE) != 0)
    {
        add(kDiffuse, 4, true);
    }

    if ((fvf & D3DFVF_SPECULAR) != 0)
    {
        add(kSpecular, 4, true);
    }

    const auto sets = static_cast<uint32_t>((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    for (uint32_t set = 0; set < sets && set < 8; ++set)
    {
        add(static_cast<Attribute>(kTexCoord0 + set), TexCoordSize(fvf, set));
    }

    return layout;
}

// The FVF a declaration-only shader's layout stands for, so the fixed-function key knows its inputs.
auto FvfFor(const std::vector<Element>& layout) -> DWORD
{
    auto fvf  = DWORD{ 0 };
    auto sets = DWORD{ 0 };
    for (const auto& element : layout)
    {
        switch (element.attribute)
        {
            case kPosition:
                if (element.components == 4)
                {
                    fvf |= D3DFVF_XYZRHW;
                }
                else
                {
                    fvf |= D3DFVF_XYZ;
                }

                break;
            case kNormal:
                fvf |= D3DFVF_NORMAL;
                break;
            case kPointSize:
                fvf |= D3DFVF_PSIZE;
                break;
            case kDiffuse:
                fvf |= D3DFVF_DIFFUSE;
                break;
            case kSpecular:
                fvf |= D3DFVF_SPECULAR;
                break;
            default:
                if (element.attribute >= kTexCoord0 && element.attribute < kTexCoord0 + 8)
                {
                    constexpr auto kCodes = std::array<DWORD, 4>{ 3, 0, 1, 2 };
                    const auto     set    = static_cast<DWORD>(element.attribute - kTexCoord0);
                    sets                  = std::max(sets, set + 1);
                    fvf |= kCodes[(element.components - 1) & 3] << (16 + set * 2);
                }

                break;
        }
    }

    return fvf | (sets << D3DFVF_TEXCOUNT_SHIFT);
}

} // namespace gl
