#include "app/boot.h"

#include "hle/boot.h"

#include <format>

namespace tmweb
{

// The stand-ins keep the boot and the folder alive; a failure halts the machine with the reason, so the host's
// first run reports it
void boot(Machine& machine, const std::filesystem::path& runtime)
{
    try
    {
        hle::Boot::start(machine, std::make_shared<hle::DiskFiles>(runtime));
    }
    catch (const std::exception& error)
    {
        machine.halt(std::format("boot: {}", error.what()));
    }
}

} // namespace tmweb
