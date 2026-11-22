#include "core/memory.h"

#include <catch2/catch_test_macros.hpp>

using namespace tmweb;

TEST_CASE("Mapped guest memory reads back what was written")
{
    auto memory = Memory{};
    memory.map(layout::kHeap, 0x2000);
    memory.write<std::uint32_t>(layout::kHeap + 0xFFE, 0x11223344);
    CHECK(memory.read<std::uint32_t>(layout::kHeap + 0xFFE) == 0x11223344);
    CHECK(memory.read<std::uint8_t>(layout::kHeap + 0x1000) == 0x22);
}

TEST_CASE("Touching an unmapped page faults at the first unmapped byte")
{
    auto memory = Memory{};
    memory.map(layout::kHeap, 0x1000);
    try
    {
        static_cast<void>(memory.read<std::uint32_t>(layout::kHeap + 0xFFE));
        FAIL("no fault");
    }
    catch (const GuestFault& fault)
    {
        CHECK(fault.address == layout::kHeap + 0x1000);
        CHECK(fault.access == Access::Read);
    }
    CHECK_THROWS_AS(memory.write<std::uint8_t>(0, 1), GuestFault);
}

TEST_CASE("Regions merge adjacent pages")
{
    auto memory = Memory{};
    memory.map(layout::kHeap, 0x1000);
    memory.map(layout::kHeap + 0x1000, 0x1800);
    memory.map(layout::kGameImage, 0x10);
    const auto regions = memory.regions();
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].address == layout::kGameImage);
    CHECK(regions[1].address == layout::kHeap);
    CHECK(regions[1].size == 0x3000);
}

TEST_CASE("Freed allocations are reused first fit, zeroed, and neighbours merge")
{
    auto       memory = Memory{};
    const auto a      = memory.allocate(0x1000);
    const auto b      = memory.allocate(0x2000);
    const auto c      = memory.allocate(0x1000);
    memory.write<std::uint32_t>(b, 0xDEADBEEF);
    memory.free(b);
    // A smaller block fits in b's place and comes back zeroed
    const auto d = memory.allocate(0x800);
    CHECK(d == b);
    CHECK(memory.read<std::uint32_t>(d) == 0);
    // The rest of b's space is still free
    const auto e = memory.allocate(0x1800);
    CHECK(e == b + 0x800);
    memory.free(d);
    memory.free(e);
    memory.free(a);
    // a and b merged: a block as big as both fits at a
    CHECK(memory.allocate(0x3000) == a);
    CHECK(c > b);
    CHECK_THROWS(memory.free(a + 4));
}

TEST_CASE("Freeing lets allocation go on past the heap's size")
{
    auto memory = Memory{};
    for (auto round = 0; round < 1000; ++round)
    {
        const auto block = memory.allocate(0x40000, 0x10000);
        memory.free(block);
    }
    CHECK(memory.allocate(16) == layout::kHeap);
}

TEST_CASE("Aligned allocations from a freed block keep the skipped part free")
{
    auto memory = Memory{};
    static_cast<void>(memory.allocate(16)); // so the freed block does not start aligned
    const auto a = memory.allocate(0x20010);
    REQUIRE(a % 0x10000 != 0);
    memory.free(a);
    const auto aligned = memory.allocate(0x100, 0x10000);
    CHECK(aligned % 0x10000 == 0);
    CHECK(memory.allocate(16) == a);
}
