#include "textures/texture_hooks.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace tmloader
{
namespace
{

constexpr DWORD uploadRva = 0x1b9aa4;

struct CallSite
{
    DWORD               rva;
    std::array<BYTE, 5> original;
};

constexpr std::array<CallSite, 7> callSites{ {
    { 0x17ba39, { 0xe8, 0x66, 0xe0, 0x03, 0x00 } },
    { 0x17c061, { 0xe8, 0x3e, 0xda, 0x03, 0x00 } },
    { 0x17c1f5, { 0xe8, 0xaa, 0xd8, 0x03, 0x00 } },
    { 0x17c393, { 0xe8, 0x0c, 0xd7, 0x03, 0x00 } },
    { 0x17c528, { 0xe8, 0x77, 0xd5, 0x03, 0x00 } },
    { 0x17ccfa, { 0xe8, 0xa5, 0xcd, 0x03, 0x00 } },
    { 0x17ce91, { 0xe8, 0x0e, 0xcc, 0x03, 0x00 } },
} };
constexpr BYTE                    uploadPrologue[]{ 0x55, 0x8b, 0xec, 0xff, 0x75, 0x40, 0x6a, 0x03 };
constexpr BYTE                    uploadReturn[]{ 0xc2, 0x3c, 0x00 };
constexpr DWORD                   uploadReturnRva = 0x1b9ae1;

// Query every region before reading: a missing or protected image is an ordinary
// unsupported-build error, not an access violation during signature checking.
auto readableRange(const BYTE* begin, SIZE_T size, bool uniformProtection = false) -> MEMORY_BASIC_INFORMATION
{
    const auto firstAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto end          = firstAddress + size;
    if (end < firstAddress)
        throw std::runtime_error("Texture hook address overflow");
    MEMORY_BASIC_INFORMATION first{};
    for (auto address = firstAddress; address < end;)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            throw std::runtime_error("Texture hook code is not readable");
        if (address == firstAddress)
            first = memory;
        else if (uniformProtection && (memory.Protect != first.Protect ||
                                       memory.AllocationBase != first.AllocationBase))
            throw std::runtime_error("Texture hook pages have incompatible protection");
        const auto next = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (next <= address)
            throw std::runtime_error("Texture hook memory range is invalid");
        address = next;
    }
    return first;
}

void restoreCalls(BYTE* image) noexcept
{
    for (const auto& site : callSites)
        std::memcpy(image + site.rva, site.original.data(), site.original.size());
}

} // namespace

auto installTextureCallHooks(BYTE* image, TextureUploadFn replacement) -> TextureUploadFn
{
    static_assert(sizeof(void*) == 4, "Native texture hooks require x86");
    if (!image || !replacement)
        throw std::invalid_argument("Texture hook target is null");
    if (reinterpret_cast<std::uintptr_t>(image) > UINTPTR_MAX - uploadReturnRva - sizeof(uploadReturn))
        throw std::runtime_error("Texture hook image address overflow");

    readableRange(image + uploadRva, sizeof(uploadPrologue));
    readableRange(image + uploadReturnRva, sizeof(uploadReturn));
    if (std::memcmp(image + uploadRva, uploadPrologue, sizeof(uploadPrologue)) ||
        std::memcmp(image + uploadReturnRva, uploadReturn, sizeof(uploadReturn)))
        throw std::runtime_error("Unsupported native texture upload function");

    BYTE* const  begin = image + callSites.front().rva;
    const SIZE_T span  = callSites.back().rva + 5 - callSites.front().rva;
    readableRange(begin, span, true);
    for (const auto& site : callSites)
    {
        if (std::memcmp(image + site.rva, site.original.data(), site.original.size()))
            throw std::runtime_error("Unsupported native texture upload call");
    }

    // These seven call sites share two code pages. One protection transaction
    // ensures an acquisition failure cannot leave partially installed hooks.
    DWORD originalProtection{};
    if (!VirtualProtect(begin, span, PAGE_EXECUTE_READWRITE, &originalProtection))
        throw std::runtime_error("Cannot make texture upload calls writable");
    for (const auto& site : callSites)
    {
        // x86 relative CALL arithmetic wraps at 32 bits, including ASLR cases.
        const auto relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(replacement)) -
                              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(image + site.rva + 5));
        std::memcpy(image + site.rva + 1, &relative, sizeof(relative));
    }

    DWORD      ignored{};
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), begin, span) != FALSE;
    if (!flushed || !VirtualProtect(begin, span, originalProtection, &ignored))
    {
        // A failed protection change leaves this entire span writable. Restore
        // executable behavior first, then make a best effort to restore protection.
        restoreCalls(image);
        FlushInstructionCache(GetCurrentProcess(), begin, span);
        VirtualProtect(begin, span, originalProtection, &ignored);
        throw std::runtime_error("Cannot finalize texture upload hooks; original calls restored");
    }
    return reinterpret_cast<TextureUploadFn>(image + uploadRva);
}

} // namespace tmloader
