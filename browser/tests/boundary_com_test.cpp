#include "core/boundary.h"

#include <catch2/catch_test_macros.hpp>

using namespace tmweb;

TEST_CASE("A guest COM object finds its host object again")
{
    auto memory   = Memory{};
    auto boundary = Boundary{ memory };
    auto host     = std::make_shared<int>(42);

    const auto object = boundary.newObject(0x12345678, host);
    CHECK(memory.read<std::uint32_t>(object) == 0x12345678);
    CHECK(boundary.object<int>(object) == host.get());

    const auto second = boundary.newObject(0x12345678, std::make_shared<int>(7));
    CHECK(second != object);
    CHECK(*boundary.object<int>(second) == 7);
}

TEST_CASE("Pointers newObject never made have no host object")
{
    auto memory   = Memory{};
    auto boundary = Boundary{ memory };
    CHECK(boundary.object<int>(0) == nullptr);
    CHECK(boundary.object<int>(layout::kHeapEnd - 4) == nullptr);

    const auto forged = memory.allocate(8);
    memory.write<std::uint32_t>(forged + 4, 5);
    CHECK(boundary.object<int>(forged) == nullptr);
}
