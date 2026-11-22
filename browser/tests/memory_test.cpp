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
