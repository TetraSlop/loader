#pragma once
#include "config/config.h"
#include <filesystem>
#include <optional>
#include <string>

namespace tmloader
{

// Signs in with LOGIN before the game starts. A configured account is tried
// first; when there is none, or the server refuses it, a small window asks
// for the username, password and server, and saves them to configFile once the
// server accepts them. Returns the configuration the game runs with, which
// holds the server's one-time game password, or nothing when the player
// closes the window.
auto signIn(const Config& config, const std::filesystem::path& configFile) -> std::optional<Config>;

// "Signed in as ..." or "Account ... created" once signed in; empty before.
auto accountStatus() -> std::string;

} // namespace tmloader
