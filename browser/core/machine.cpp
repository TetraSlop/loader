#include "core/machine.h"

#include <stdexcept>

namespace tmweb
{

Machine::Machine()
: cpu_(memory_)
, boundary_(memory_)
, scheduler_(memory_)
{
}

auto Machine::memory() -> Memory&
{
    return memory_;
}

auto Machine::cpu() -> Cpu&
{
    return cpu_;
}

auto Machine::boundary() -> Boundary&
{
    return boundary_;
}

auto Machine::scheduler() -> Scheduler&
{
    return scheduler_;
}

auto Machine::run(std::uint64_t) -> RunResult
{
    return RunResult{ true, "Machine::run is not implemented yet" };
}

auto Machine::callGuest(std::uint32_t, std::span<const std::uint32_t>, Convention) -> std::uint32_t
{
    throw std::logic_error("Machine::callGuest is not implemented yet");
}

auto Machine::raise(std::uint32_t, std::uint32_t, std::span<const std::uint32_t>) -> bool
{
    return false;
}

void Machine::halt(std::string reason)
{
    halted_ = true;
    reason_ = std::move(reason);
}

} // namespace tmweb
