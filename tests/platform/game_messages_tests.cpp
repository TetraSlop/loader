#include "app/runtime.h"
#include "platform/game_messages.h"
#include "platform/patch.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace tmloader
{

Config options;

void writeLog(std::string_view)
{
}

} // namespace tmloader

using namespace tmloader;

TEST_CASE("Match messages drive the game phase", "[game_messages]")
{
    auto       phase = GamePhase::menus;
    const auto step  = [&](std::string_view message)
    {
        phase = nextPhase(phase, message);
        return phase;
    };

    REQUIRE(step("GTM0G1E000000@Pong") == GamePhase::menus);
    REQUIRE(step("GTM0L\aRE\a1") == GamePhase::lobby);
    REQUIRE(step("GTM0G43000100@VsGameInit=/N=2") == GamePhase::match);
    REQUIRE(step("GTM0G43000900@TurnData=/A=0") == GamePhase::match);
    REQUIRE(step("GTM0G43000C00@BattleData=") == GamePhase::match);
    REQUIRE(step("GTM0G43000D09@ResultData=") == GamePhase::results);
    REQUIRE(step("GTM0G43001101@Continue=/Ans=1|1/B=0|0") == GamePhase::match);
    REQUIRE(step("GTM0G43000D09@ResultData=") == GamePhase::results);
    REQUIRE(step("GTM0G43001102@Continue=/Ans=3|3/B=0|0") == GamePhase::lobby);
    REQUIRE(step("GTM0L\aRE\a1") == GamePhase::lobby);
    REQUIRE(step("GTM0G43002500@WatchInfo=") == GamePhase::watching);
    REQUIRE(step("GTM0G4300050A@Quit=/No=0/B=0") == GamePhase::lobby);
}

TEST_CASE("Unrelated or malformed messages keep the phase", "[game_messages]")
{
    for (const auto* const message : { "", "GTM0", "GTM", "PING :x", "GTM0GB2000100@Shop=", "GTM0G43", "GTM0G43zz0900", "GTM0G43001A09@TCList=" })
    {
        CAPTURE(message);
        REQUIRE(nextPhase(GamePhase::match, message) == GamePhase::match);
    }
}

TEST_CASE("Call redirection checks the original target", "[game_messages]")
{
    // A code page with CALL +0x10 at offset 0x20, i.e. to offset 0x35.
    auto* const code = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
    REQUIRE(code);
    auto old = DWORD{};
    VirtualProtect(code, 0x1000, PAGE_EXECUTE_READWRITE, &old);
    const BYTE call[] = { 0xe8, 0x10, 0x00, 0x00, 0x00 };
    std::memcpy(code + 0x20, call, sizeof(call));
    VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old);
    const auto module = reinterpret_cast<HMODULE>(code);

    REQUIRE_THROWS(redirectCall(module, 0x20, 0x40, code + 0x100));
    REQUIRE_THROWS(redirectCall(module, 0x21, 0x35, code + 0x100));
    REQUIRE(redirectCall(module, 0x20, 0x35, code + 0x100) == code + 0x35);
    auto relative = std::int32_t{};
    std::memcpy(&relative, code + 0x21, sizeof(relative));
    REQUIRE(code + 0x25 + relative == code + 0x100);
    VirtualFree(code, 0, MEM_RELEASE);
}
