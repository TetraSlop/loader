#include "platform/patch.h"
#include "app/log.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>

namespace tmloader
{

void replacePointer(HMODULE module, void* oldValue, void* newValue)
{
    auto* const       base = reinterpret_cast<BYTE*>(module);
    const auto* const dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto        size = nt->OptionalHeader.SizeOfImage;
    for (DWORD rva = 0x1000; rva + sizeof(void*) < size;)
    {
        auto memory = MEMORY_BASIC_INFORMATION{};
        if (!VirtualQuery(base + rva, &memory, sizeof(memory)))
        {
            break;
        }

        const auto regionEnd = static_cast<BYTE*>(memory.BaseAddress) + memory.RegionSize;
        const auto end       = std::min(size, static_cast<DWORD>(regionEnd - base));
        if (end <= rva)
        {
            break;
        }

        if (memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            for (; rva + sizeof(void*) <= end; rva += sizeof(void*))
            {
                auto* const slot = reinterpret_cast<void**>(base + rva);
                auto        old  = DWORD{};
                if (*slot == oldValue && VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
                {
                    *slot = newValue;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                }
            }
        }

        rva = end;
    }
}

void setImport(HMODULE module, DWORD rva, void* function)
{
    auto* const slot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(module) + rva);
    auto        old  = DWORD{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
    {
        throw std::runtime_error("Cannot make import slot writable");
    }

    logLine("Replace import +{:X}: {} -> {}", rva, *slot, function);
    *slot = function;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

auto redirectCall(HMODULE module, DWORD callRva, DWORD targetRva, void* replacement) -> void*
{
    auto* const base     = reinterpret_cast<BYTE*>(module);
    auto* const site     = base + callRva;
    auto        relative = std::int32_t{};
    std::memcpy(&relative, site + 1, sizeof(relative));
    if (site[0] != 0xe8 || site + 5 + relative != base + targetRva)
    {
        throw std::runtime_error(std::format("Unsupported call at +{:X}", callRva));
    }

    auto protection = DWORD{};
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protection))
    {
        throw std::runtime_error("Cannot make call site writable");
    }

    // x86 relative CALL arithmetic wraps at 32 bits.
    const auto redirected = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(replacement)) -
                            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(site + 5));
    std::memcpy(site + 1, &redirected, sizeof(redirected));
    VirtualProtect(site, 5, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return base + targetRva;
}

} // namespace tmloader
