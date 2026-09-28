#pragma once
#include "config/config.h"
#include "hosted/session.h"
#include <memory>

namespace tmloader
{

// Hosted-mode launcher: Sign in with Discord, then Play. Returns a session whose
// play session and tunnel are running, or nothing when the user closes the launcher.
auto runLauncher(const Config& config) -> std::unique_ptr<hosted::HostedSession>;

} // namespace tmloader
