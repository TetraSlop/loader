#pragma once
#include <functional>
#include <string_view>

namespace tmloader
{

enum class GamePhase
{
    menus,
    lobby,
    match,
    results,
    watching,
};

// Advances the phase from one incoming GTM0 game message. Match messages are
// family 0x43; see TMPackets game/Match.md for the subtypes.
auto nextPhase(GamePhase current, std::string_view message) -> GamePhase;
auto describePhase(GamePhase phase) -> const char*;

// Called on the game thread whenever the phase changes; keep it quick.
void setGamePhaseListener(std::function<void(GamePhase)> listener);

} // namespace tmloader
