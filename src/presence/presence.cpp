#include "presence/presence.h"
#include "presence/discord_presence.h"
#include "presence/game_phase.h"
#include <chrono>

namespace tmloader
{
namespace
{

// Discord shows the time since startedAt, so the timer restarts with each phase.
auto activityFor(const Config& config, GamePhase phase) -> Activity
{
    const auto startedAt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto       activity  = Activity{ "Playing on a local server", describePhase(phase), startedAt, "logo", "Tetra Master" };
    if (!config.hostedServer.empty())
    {
        activity.details = "Playing online on " + config.hostedServer.substr(config.hostedServer.find("://") + 3);
    }

    return activity;
}

} // namespace

void startPresence(const Config& config)
{
    if (config.discordApplicationId.empty())
    {
        return;
    }

    static auto discord = DiscordPresence(config.discordApplicationId);
    discord.set(activityFor(config, GamePhase::menus));
    setGamePhaseListener([config](GamePhase phase)
                         {
                             discord.set(activityFor(config, phase));
                         });
}

} // namespace tmloader
