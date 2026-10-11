#include "platform/patch.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace
{

constexpr DWORD kPageSize    = 0x1000;
constexpr DWORD kCallSite    = 0x20;
constexpr DWORD kCallTarget  = 0x35;
constexpr DWORD kReplacement = 0x100;
// CALL +0x10 at kCallSite lands on kCallTarget.
constexpr BYTE kCall[] = { 0xe8, 0x10, 0x00, 0x00, 0x00 };

} // namespace

namespace tmloader
{

void writeLog(std::string_view)
{
}

} // namespace tmloader

using namespace tmloader;

namespace
{

// A read-only executable page holding the one call, like the game's code section.
auto allocateCode() -> BYTE*
{
    auto* const code = static_cast<BYTE*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    REQUIRE(code);
    std::memcpy(code + kCallSite, kCall, sizeof(kCall));
    auto old = DWORD{};
    VirtualProtect(code, kPageSize, PAGE_EXECUTE_READ, &old);
    return code;
}

} // namespace

TEST_CASE("redirectCall leaves the code alone unless the call and its target match", "[patch]")
{
    auto* const code   = allocateCode();
    const auto  module = reinterpret_cast<HMODULE>(code);

    REQUIRE_THROWS(redirectCall(module, kCallSite, 0x40, code + kReplacement));
    REQUIRE_THROWS(redirectCall(module, kCallSite + 1, kCallTarget, code + kReplacement));
    REQUIRE(std::memcmp(code + kCallSite, kCall, sizeof(kCall)) == 0);
    VirtualFree(code, 0, MEM_RELEASE);
}

TEST_CASE("redirectCall retargets the call and returns the original target", "[patch]")
{
    auto* const code   = allocateCode();
    const auto  module = reinterpret_cast<HMODULE>(code);

    REQUIRE(redirectCall(module, kCallSite, kCallTarget, code + kReplacement) == code + kCallTarget);
    auto relative = std::int32_t{};
    std::memcpy(&relative, code + kCallSite + 1, sizeof(relative));
    const BYTE* const next = code + kCallSite + sizeof(kCall);
    REQUIRE(next + relative == code + kReplacement);

    auto memory = MEMORY_BASIC_INFORMATION{};
    VirtualQuery(code, &memory, sizeof(memory));
    REQUIRE(memory.Protect == PAGE_EXECUTE_READ);
    VirtualFree(code, 0, MEM_RELEASE);
}
