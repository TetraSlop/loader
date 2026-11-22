#include "platform/patch.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string_view>

namespace
{

constexpr DWORD kPageSize = 0x1000;
constexpr DWORD kSite     = 0x20;
constexpr BYTE  kJump[]   = { 0xe9, 0x10, 0x00, 0x00, 0x00 };

} // namespace

namespace tmloader
{

void writeLog(std::string_view)
{
}

} // namespace tmloader

TEST_CASE("writeCode patches a read-only code page and restores its protection", "[patch]")
{
    auto* const code = static_cast<BYTE*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
    REQUIRE(code);

    tmloader::writeCode(code + kSite, kJump);
    REQUIRE(std::memcmp(code + kSite, kJump, sizeof(kJump)) == 0);

    auto memory = MEMORY_BASIC_INFORMATION{};
    VirtualQuery(code, &memory, sizeof(memory));
    REQUIRE(memory.Protect == PAGE_EXECUTE_READ);
    VirtualFree(code, 0, MEM_RELEASE);
}
