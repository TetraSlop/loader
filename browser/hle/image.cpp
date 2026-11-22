#include "hle/image.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

namespace tmweb::hle
{
namespace
{

template <class T>
auto at(const std::span<const std::uint8_t> file, const std::size_t offset) -> T
{
    if (offset + sizeof(T) > file.size())
    {
        throw std::runtime_error("Truncated PE file");
    }
    T value;
    std::memcpy(&value, file.data() + offset, sizeof(T));
    return value;
}

} // namespace

auto loadImage(Memory& memory, Boundary& boundary, const std::span<const std::uint8_t> file, const std::uint32_t base) -> Image
{
    if (at<std::uint16_t>(file, 0) != 0x5A4D)
    {
        throw std::runtime_error("Not a PE file");
    }
    const auto pe = at<std::uint32_t>(file, 0x3C);
    if (at<std::uint32_t>(file, pe) != 0x4550 || at<std::uint16_t>(file, pe + 4) != 0x014C)
    {
        throw std::runtime_error("Not a 32-bit x86 PE file");
    }
    const auto sections    = at<std::uint16_t>(file, pe + 6);
    const auto optional    = pe + 24;
    const auto sectionBase = optional + at<std::uint16_t>(file, pe + 20);

    auto       image     = Image{};
    const auto preferred = at<std::uint32_t>(file, optional + 28);
    image.base           = base ? base : preferred;
    image.size           = (at<std::uint32_t>(file, optional + 56) + kPageSize - 1) & ~(kPageSize - 1);
    const auto entry     = at<std::uint32_t>(file, optional + 16);
    image.entry          = entry ? image.base + entry : 0;
    const auto headers   = std::min<std::size_t>(at<std::uint32_t>(file, optional + 60), file.size());
    memory.map(image.base, image.size);
    memory.write(image.base, file.first(headers));

    for (auto i = 0u; i < sections; ++i)
    {
        const auto header      = sectionBase + 40 * i;
        const auto virtualSize = at<std::uint32_t>(file, header + 8);
        const auto address     = at<std::uint32_t>(file, header + 12);
        const auto rawSize     = at<std::uint32_t>(file, header + 16);
        const auto raw         = at<std::uint32_t>(file, header + 20);
        const auto size        = std::min<std::size_t>({ virtualSize ? virtualSize : rawSize, rawSize, file.size() - std::min<std::size_t>(raw, file.size()) });
        memory.write(image.base + address, file.subspan(raw, size));
    }

    // IMAGE_BASE_RELOCATION blocks: page RVA, block size, then 16-bit entries (type 3: a 32-bit address)
    const auto relocations = at<std::uint32_t>(file, optional + 136);
    const auto relocated   = at<std::uint32_t>(file, optional + 140);
    if (image.base != preferred && relocations)
    {
        const auto delta = image.base - preferred;
        for (auto block = image.base + relocations; block < image.base + relocations + relocated;)
        {
            const auto page = memory.read<std::uint32_t>(block);
            const auto size = memory.read<std::uint32_t>(block + 4);
            if (size < 8)
            {
                break;
            }
            for (auto item = block + 8; item < block + size; item += 2)
            {
                const auto value = memory.read<std::uint16_t>(item);
                if (value >> 12 == 3)
                {
                    const auto address = image.base + page + (value & 0xFFF);
                    memory.write<std::uint32_t>(address, memory.read<std::uint32_t>(address) + delta);
                }
            }
            block += size;
        }
    }

    // IMAGE_IMPORT_DESCRIPTORs: OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk
    const auto imports = at<std::uint32_t>(file, optional + 104);
    for (auto descriptor = image.base + imports; imports && memory.read<std::uint32_t>(descriptor + 12); descriptor += 20)
    {
        const auto module = memory.readString(image.base + memory.read<std::uint32_t>(descriptor + 12));
        const auto first  = memory.read<std::uint32_t>(descriptor + 16);
        const auto names  = memory.read<std::uint32_t>(descriptor) ? memory.read<std::uint32_t>(descriptor) : first;
        for (auto slot = 0u;; slot += 4)
        {
            const auto thunk = memory.read<std::uint32_t>(image.base + names + slot);
            if (thunk == 0)
            {
                break;
            }
            const auto name = thunk & 0x80000000 ? std::format("#{}", thunk & 0xFFFF) : memory.readString(image.base + thunk + 2);
            memory.write<std::uint32_t>(image.base + first + slot, boundary.resolve(module, name));
        }
    }
    return image;
}

auto findExport(const Memory& memory, const std::uint32_t base, const std::string_view name) -> std::uint32_t
{
    if (!memory.mapped(base, 0x40) || memory.read<std::uint16_t>(base) != 0x5A4D)
    {
        return 0;
    }
    const auto pe        = base + memory.read<std::uint32_t>(base + 0x3C);
    const auto directory = memory.read<std::uint32_t>(pe + 0x78);
    if (directory == 0)
    {
        return 0;
    }
    const auto exports     = base + directory;
    const auto ordinalBase = memory.read<std::uint32_t>(exports + 16);
    const auto count       = memory.read<std::uint32_t>(exports + 20);
    const auto functions   = base + memory.read<std::uint32_t>(exports + 28);
    const auto function    = [&](const std::uint32_t index) -> std::uint32_t
    {
        if (index >= count)
        {
            return 0;
        }
        const auto rva = memory.read<std::uint32_t>(functions + 4 * index);
        return rva ? base + rva : 0;
    };

    if (name.starts_with('#'))
    {
        auto ordinal = std::uint32_t{};
        std::from_chars(name.data() + 1, name.data() + name.size(), ordinal);
        return ordinal >= ordinalBase ? function(ordinal - ordinalBase) : 0;
    }
    const auto names    = memory.read<std::uint32_t>(exports + 24);
    const auto table    = base + memory.read<std::uint32_t>(exports + 32);
    const auto ordinals = base + memory.read<std::uint32_t>(exports + 36);
    for (auto i = 0u; i < names; ++i)
    {
        if (memory.readString(base + memory.read<std::uint32_t>(table + 4 * i)) == name)
        {
            return function(memory.read<std::uint16_t>(ordinals + 2 * i));
        }
    }
    return 0;
}

} // namespace tmweb::hle
