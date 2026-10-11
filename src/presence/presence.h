#pragma once
#include <windows.h>

#include "config/config.h"

// Discord Rich Presence. Optional: built when TMLOADER_DISCORD_PRESENCE is on.
namespace tmloader
{

// Connects to Discord when an application ID is configured. Call before the
// process lowers its integrity, which would block opening Discord's pipe.
void startPresence(const Config& config);
// Makes the presence follow the game between menus, lobby and matches.
void installPresenceHooks(HMODULE game);

} // namespace tmloader
