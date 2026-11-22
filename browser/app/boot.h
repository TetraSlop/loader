#pragma once

// Load the runtime folder into a fresh machine: map TM_native.dll and polcore, register the stand-ins, publish them
// and set up the main thread, so Machine::run starts at the game's entry. The host calls it from tmweb_boot.

#include "core/machine.h"

#include <filesystem>

namespace tmweb
{

void boot(Machine& machine, const std::filesystem::path& runtime);

} // namespace tmweb
