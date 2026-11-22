#include "app/boot.h"

#include "hle/boot.h"

#include "host/host.h"

#ifdef __EMSCRIPTEN__
#include "com/directx.h"
#include "com/proxies.h"
#endif

#include <format>
#include <optional>

namespace tmweb
{

static_assert(static_cast<int>(host::InputKind::Key) == static_cast<int>(hle::InputKind::Key) && static_cast<int>(host::InputKind::Move) == static_cast<int>(hle::InputKind::Move) &&
              static_cast<int>(host::InputKind::Button) == static_cast<int>(hle::InputKind::Button) && static_cast<int>(host::InputKind::Wheel) == static_cast<int>(hle::InputKind::Wheel));

// The stand-ins keep the boot and the folder alive; a failure halts the machine with the reason, so the host's
// first run reports it. The host's keyboard and mouse go to user32's queue, and what the game writes to the host's
// copy of the folder.
void boot(Machine& machine, const std::filesystem::path& runtime, const hle::BootOptions& options)
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
        auto files         = std::make_shared<hle::DiskFiles>(runtime);
        files->written     = host::fileWritten;
        const auto started = std::weak_ptr<hle::Boot>{ hle::Boot::start(machine, std::move(files), options) };
        host::onInput([started](const host::InputEvent& event)
                      {
                          if (const auto boot = started.lock())
                          {
                              boot->process().input(static_cast<hle::InputKind>(event.kind), event.a, event.b);
                          }
                      });
    }
    catch (const std::exception& error)
    {
        machine.halt(std::format("boot: {}", error.what()));
    }
}

} // namespace tmweb
