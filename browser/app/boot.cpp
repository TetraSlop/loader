#include "app/boot.h"

#include "hle/boot.h"

#ifdef __EMSCRIPTEN__
#include "com/directx.h"
#include "com/proxies.h"
#include "host/host.h"
#endif

#include <format>
#include <optional>

namespace tmweb
{

// The stand-ins keep the boot and the folder alive; a failure halts the machine with the reason, so the host's
// first run reports it
void boot(Machine& machine, const std::filesystem::path& runtime)
{
    try
    {
#ifdef __EMSCRIPTEN__
        // d3d8, dinput8 and dsound on compat's objects. The exports keep the proxies alive with the machine.
        const auto proxies = std::make_shared<Proxies>(machine);
        auto       media   = MediaHost{};
        media.sound        = host::playSound;
        media.soundRate    = host::kSoundRate;
        // Each Present ends the run slice, so each host task shows one frame
        media.presented = host::presented;
        media.pointer   = []
        {
            const auto& input = host::inputState();
            return std::optional<Pointer>{ Pointer{ input.x, input.y } };
        };
        media.gameCursor = layout::kGameImage + 0x2FF570;
        for (auto function : addDirectX(*proxies, std::move(media)))
        {
            function.handler = [proxies, inner = std::move(function.handler)](Call& call)
            {
                return inner(call);
            };
            machine.boundary().add(std::move(function));
        }
#endif
        hle::Boot::start(machine, std::make_shared<hle::DiskFiles>(runtime));
    }
    catch (const std::exception& error)
    {
        machine.halt(std::format("boot: {}", error.what()));
    }
}

} // namespace tmweb
