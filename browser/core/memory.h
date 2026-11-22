#pragma once

// Guest memory. Guest addresses are host addresses offset by base(): in the Wasm build base() is 0, so a pointer the
// game stores can be handed to the compat layer unchanged. The native build reserves 4 GiB and offsets into it.
// Pages are 4 KiB and either mapped (read, write and execute) or not; touching an unmapped page throws GuestFault.

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace tmweb
{

// Where things live in the guest. Everything is inside the arena, above the host's own heap.
namespace layout
{

constexpr std::uint32_t kArenaBase   = 0x10000000;
constexpr std::uint32_t kArenaEnd    = 0x18000000;
constexpr std::uint32_t kGameImage   = 0x10000000; // TM.dll's pinned base
constexpr std::uint32_t kViewerImage = 0x10400000; // polcore, relocated
constexpr std::uint32_t kModules     = 0x10A00000; // synthetic system DLL images and their stand-in entry points
constexpr std::uint32_t kModulesEnd  = 0x10C00000;
constexpr std::uint32_t kThreads     = 0x10C00000; // TEBs and stacks
constexpr std::uint32_t kThreadsEnd  = 0x11000000;
constexpr std::uint32_t kHeap        = 0x11000000; // VirtualAlloc regions and the process heap
constexpr std::uint32_t kHeapEnd     = kArenaEnd;

} // namespace layout

enum : std::uint32_t
{
    kPageSize = 0x1000,
};

enum class Access : std::uint8_t
{
    Read,
    Write,
    Execute,
};

// Thrown by every checked access to an unmapped page; the CPU turns it into an access violation.
struct GuestFault
{
    std::uint32_t address = 0;
    Access        access  = Access::Read;
};

struct Region
{
    std::uint32_t address = 0;
    std::uint32_t size    = 0;
};

class Memory final
{
public:
    Memory();
    ~Memory();
    Memory(const Memory&)                    = delete;
    Memory(Memory&&)                         = delete;
    auto operator=(const Memory&) -> Memory& = delete;
    auto operator=(Memory&&) -> Memory&      = delete;

    // Map zero-filled pages covering [address, address + size). Mapping a mapped page keeps its contents.
    void map(std::uint32_t address, std::uint32_t size);
    void unmap(std::uint32_t address, std::uint32_t size);
    auto mapped(std::uint32_t address, std::uint32_t size = 1) const -> bool;
    // Mapped ranges, merged, for snapshots and the differential test
    auto regions() const -> std::vector<Region>;

    // Allocate address space from the heap part of the arena, mapped and zeroed; never reused. Alignment is a power
    // of two; VirtualAlloc uses 64 KiB.
    auto allocate(std::uint32_t size, std::uint32_t alignment = 16) -> std::uint32_t;

    // Unchecked host view of a guest address
    auto host(const std::uint32_t address) const -> std::uint8_t*
    {
        return reinterpret_cast<std::uint8_t*>(base_ + address);
    }

    auto base() const -> std::uintptr_t
    {
        return base_;
    }

    // Checked access; throws GuestFault
    template <class T>
    auto read(const std::uint32_t address) const -> T
    {
        check(address, sizeof(T), Access::Read);
        T value;
        std::memcpy(&value, host(address), sizeof(T));
        return value;
    }

    template <class T>
    void write(const std::uint32_t address, const T value)
    {
        check(address, sizeof(T), Access::Write);
        std::memcpy(host(address), &value, sizeof(T));
    }

    void read(std::uint32_t address, std::span<std::uint8_t> out) const;
    void write(std::uint32_t address, std::span<const std::uint8_t> data);
    void fill(std::uint32_t address, std::uint8_t value, std::uint32_t size);
    // A NUL-terminated byte string (Shift-JIS stays as bytes); faults past the end of mapped memory
    auto readString(std::uint32_t address, std::uint32_t limit = 0x10000) const -> std::string;
    auto readWideString(std::uint32_t address, std::uint32_t limit = 0x10000) const -> std::u16string;
    void writeString(std::uint32_t address, std::string_view text); // writes the terminator too

    void check(const std::uint32_t address, const std::uint32_t size, const Access access) const
    {
        const auto first = address >> 12;
        const auto last  = (address + size - 1) >> 12;
        if (!pages_[first] || !pages_[last] || last < first)
        {
            fault(address, size, access);
        }
    }

private:
    [[noreturn]] void fault(std::uint32_t address, std::uint32_t size, Access access) const;

    std::uintptr_t            base_ = 0;
    std::vector<std::uint8_t> pages_; // one byte per 4 KiB page: 1 when mapped
    std::uint32_t             heapTop_ = layout::kHeap;
};

} // namespace tmweb
