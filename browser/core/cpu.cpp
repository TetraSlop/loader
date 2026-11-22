#include "core/cpu.h"

namespace tmweb
{

Cpu::Cpu(Memory& memory)
: memory_(memory)
{
}

auto Cpu::run(Context& context, const std::uint64_t budget) -> Event
{
    for (auto count = std::uint64_t{}; count < budget; ++count)
    {
        const auto event = step(context);
        if (event.stop != Stop::Budget)
        {
            return event;
        }
    }
    return Event{ Stop::Budget, context.eip };
}

auto Cpu::step(Context& context) -> Event
{
    if (traps_.contains(context.eip))
    {
        return Event{ Stop::Trap, context.eip };
    }
    // The decoder lands in the next commit
    return Event{ Stop::Interrupt, context.eip, 6 };
}

void Cpu::setTraps(const TrapRange traps)
{
    traps_ = traps;
}

auto Cpu::retired() const -> std::uint64_t
{
    return retired_;
}

} // namespace tmweb
