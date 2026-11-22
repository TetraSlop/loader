#include "core/memory.h"

#include <stdexcept>

#if defined(__EMSCRIPTEN__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace tmweb
{
namespace
{

constexpr std::uint64_t kAddressSpace = 0x100000000ULL;
constexpr std::uint32_t kPageCount    = static_cast<std::uint32_t>(kAddressSpace / kPageSize);

auto pageFloor(const std::uint32_t address) -> std::uint32_t
{
    return address & ~(kPageSize - 1);
}

// The Wasm build keeps guest addresses equal to linear-memory addresses. The host heap must stay below the arena:
// move the program break past the arena once, so later host allocations land above it.
auto reserveAddressSpace() -> std::uintptr_t
{
#if defined(__EMSCRIPTEN__)
    const auto current = reinterpret_cast<std::uintptr_t>(sbrk(0));
    if (current > layout::kArenaBase)
    {
        throw std::runtime_error("The host heap already reaches the guest arena");
    }
    if (sbrk(static_cast<intptr_t>(layout::kArenaEnd - current)) == reinterpret_cast<void*>(-1))
    {
        throw std::runtime_error("Cannot grow linear memory to hold the guest arena");
    }
    return 0;
#elif defined(_WIN32)
    auto* const base = VirtualAlloc(nullptr, kAddressSpace, MEM_RESERVE, PAGE_NOACCESS);
    if (!base)
    {
        throw std::runtime_error("Cannot reserve 4 GiB for guest memory");
    }
    return reinterpret_cast<std::uintptr_t>(base);
#else
    auto* const base = mmap(nullptr, kAddressSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
    {
        throw std::runtime_error("Cannot reserve 4 GiB for guest memory");
    }
    return reinterpret_cast<std::uintptr_t>(base);
#endif
}

void releaseAddressSpace(const std::uintptr_t base)
{
#if defined(__EMSCRIPTEN__)
    static_cast<void>(base);
#elif defined(_WIN32)
    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
#else
    munmap(reinterpret_cast<void*>(base), kAddressSpace);
#endif
}

void commit(const std::uintptr_t base, const std::uint32_t address, const std::uint32_t size)
{
#if defined(__EMSCRIPTEN__)
    if (address < layout::kArenaBase || static_cast<std::uint64_t>(address) + size > layout::kArenaEnd)
    {
        throw std::runtime_error("Guest mapping outside the arena");
    }
    std::memset(reinterpret_cast<void*>(base + address), 0, size);
#elif defined(_WIN32)
    if (!VirtualAlloc(reinterpret_cast<void*>(base + address), size, MEM_COMMIT, PAGE_READWRITE))
    {
        throw std::runtime_error("Cannot commit guest memory");
    }
#else
    if (mprotect(reinterpret_cast<void*>(base + address), size, PROT_READ | PROT_WRITE) != 0)
    {
        throw std::runtime_error("Cannot commit guest memory");
    }
#endif
}

} // namespace

Memory::Memory()
: base_(reserveAddressSpace())
, pages_(kPageCount, 0)
{
}

Memory::~Memory()
{
    releaseAddressSpace(base_);
}

void Memory::map(const std::uint32_t address, const std::uint32_t size)
{
    const auto first = pageFloor(address);
    const auto end   = static_cast<std::uint64_t>(address) + size;
    for (auto page = static_cast<std::uint64_t>(first); page < end; page += kPageSize)
    {
        const auto index = static_cast<std::uint32_t>(page >> 12);
        if (!pages_[index])
        {
            commit(base_, static_cast<std::uint32_t>(page), kPageSize);
            pages_[index] = 1;
        }
    }
}

void Memory::unmap(const std::uint32_t address, const std::uint32_t size)
{
    const auto end = static_cast<std::uint64_t>(address) + size;
    for (auto page = static_cast<std::uint64_t>(pageFloor(address)); page < end; page += kPageSize)
    {
        // The pages stay committed; only the guest loses access
        pages_[static_cast<std::uint32_t>(page >> 12)] = 0;
    }
}

auto Memory::mapped(const std::uint32_t address, const std::uint32_t size) const -> bool
{
    const auto end = static_cast<std::uint64_t>(address) + size;
    for (auto page = static_cast<std::uint64_t>(pageFloor(address)); page < end; page += kPageSize)
    {
        if (!pages_[static_cast<std::uint32_t>(page >> 12)])
        {
            return false;
        }
    }
    return true;
}

auto Memory::regions() const -> std::vector<Region>
{
    auto out = std::vector<Region>{};
    for (auto index = std::uint32_t{}; index < kPageCount; ++index)
    {
        if (!pages_[index])
        {
            continue;
        }
        const auto address = index << 12;
        if (!out.empty() && out.back().address + out.back().size == address)
        {
            out.back().size += kPageSize;
        }
        else
        {
            out.push_back(Region{ address, kPageSize });
        }
    }
    return out;
}

auto Memory::allocate(const std::uint32_t size, const std::uint32_t alignment) -> std::uint32_t
{
    const auto address = (heapTop_ + alignment - 1) & ~(alignment - 1);
    if (static_cast<std::uint64_t>(address) + size > layout::kHeapEnd)
    {
        throw std::runtime_error("Guest heap exhausted");
    }
    heapTop_ = address + size;
    map(address, size);
    fill(address, 0, size);
    return address;
}

void Memory::read(const std::uint32_t address, const std::span<std::uint8_t> out) const
{
    if (out.empty())
    {
        return;
    }
    check(address, static_cast<std::uint32_t>(out.size()), Access::Read);
    std::memcpy(out.data(), host(address), out.size());
}

void Memory::write(const std::uint32_t address, const std::span<const std::uint8_t> data)
{
    if (data.empty())
    {
        return;
    }
    check(address, static_cast<std::uint32_t>(data.size()), Access::Write);
    std::memcpy(host(address), data.data(), data.size());
}

void Memory::fill(const std::uint32_t address, const std::uint8_t value, const std::uint32_t size)
{
    if (size == 0)
    {
        return;
    }
    check(address, size, Access::Write);
    std::memset(host(address), value, size);
}

auto Memory::readString(const std::uint32_t address, const std::uint32_t limit) const -> std::string
{
    auto out = std::string{};
    for (auto offset = std::uint32_t{}; offset < limit; ++offset)
    {
        const auto c = read<char>(address + offset);
        if (c == '\0')
        {
            break;
        }
        out.push_back(c);
    }
    return out;
}

auto Memory::readWideString(const std::uint32_t address, const std::uint32_t limit) const -> std::u16string
{
    auto out = std::u16string{};
    for (auto offset = std::uint32_t{}; offset < limit; ++offset)
    {
        const auto c = read<char16_t>(address + 2 * offset);
        if (c == u'\0')
        {
            break;
        }
        out.push_back(c);
    }
    return out;
}

void Memory::writeString(const std::uint32_t address, const std::string_view text)
{
    write(address, std::span{ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    write<std::uint8_t>(address + static_cast<std::uint32_t>(text.size()), 0);
}

void Memory::fault(const std::uint32_t address, const std::uint32_t size, const Access access) const
{
    // Report the first unmapped byte, as the hardware does for an access that straddles pages
    auto at = address;
    if (pages_[address >> 12])
    {
        at = pageFloor(address + size - 1);
    }
    throw GuestFault{ at, access };
}

} // namespace tmweb
