#include "app/runtime.h"
#include "presence/game_phase.h"
#include <catch2/catch_test_macros.hpp>

namespace tmloader
{

Config options;

} // namespace tmloader

using namespace tmloader;

TEST_CASE("Match messages drive the game phase", "[presence]")
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

TEST_CASE("Unrelated or malformed messages keep the phase", "[presence]")
{
    for (const auto* const message : { "", "GTM0", "GTM", "PING :x", "GTM0GB2000100@Shop=", "GTM0G43", "GTM0G43zz0900", "GTM0G43001A09@TCList=" })
    {
        CAPTURE(message);
        REQUIRE(nextPhase(GamePhase::match, message) == GamePhase::match);
    }
}
