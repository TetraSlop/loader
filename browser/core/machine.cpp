#include "core/machine.h"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>
#include <variant>

namespace tmweb
{
namespace
{

// Instructions a thread runs before the scheduler looks at the others
constexpr std::uint64_t kSlice = 50000;

enum : std::uint32_t
{
    kArrayBounds = 0xC000008C,
};

// ExceptionInformation[0] of an access violation
auto accessKind(const Access access) -> std::uint32_t
{
    switch (access)
    {
        case Access::Write:
            return 1;
        case Access::Execute:
            return 8;
        default:
            return 0;
    }
}

auto accessName(const Access access) -> const char*
{
    switch (access)
    {
        case Access::Write:
            return "writing";
        case Access::Execute:
            return "executing";
        default:
            return "reading";
    }
}

} // namespace

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

auto Machine::run(const std::uint64_t budget) -> RunResult
{
    auto       result = RunResult{};
    const auto start  = exceptions_;
    cpu_.setTraps(boundary_.traps());
    auto remaining = budget;
    while (remaining > 0 && !halted_)
    {
        if (!scheduler_.schedule())
        {
            if (scheduler_.waiting() > 0)
            {
                result.idle = true;
                if (const auto deadline = scheduler_.nextDeadline())
                {
                    result.nextDeadlineMs = static_cast<std::uint32_t>(*deadline);
                }
            }
            else
            {
                halt("No guest thread can run");
            }
            break;
        }
        auto&      thread = scheduler_.current();
        const auto before = cpu_.retired();
        const auto event  = cpu_.run(thread.context, std::min(remaining, kSlice));
        const auto ran    = cpu_.retired() - before;
        result.instructions += ran;
        remaining -= std::min(ran, remaining);
        if (!handle(thread, event))
        {
            break;
        }
    }
    result.halted     = halted_;
    result.reason     = reason_;
    result.exceptions = exceptions_ - start;
    return result;
}

auto Machine::handle(Thread& thread, const Event& event) -> bool
{
    switch (event.stop)
    {
        case Stop::Budget:
            return true;
        case Stop::Trap:
            return dispatchTrap(thread, event.address);
        case Stop::Fault:
        case Stop::Interrupt:
            return exception(thread, event);
        default:
            halt(std::format("HLT at {:#x}", event.address));
            return false;
    }
}

auto Machine::dispatchTrap(Thread& thread, const std::uint32_t trap) -> bool
{
    if (nested_ == 0 && trap == boundary_.returnTrap())
    {
        halt("Guest code returned to the host outside callGuest");
        return false;
    }
    auto& context = thread.context;
    auto  outcome = boundary_.dispatch(*this, context, trap);
    if (halted_)
    {
        return false;
    }
    if (const auto* const done = std::get_if<Return>(&outcome))
    {
        complete(context, trap, *done);
        return !halted_;
    }
    if (auto* const block = std::get_if<Block>(&outcome))
    {
        if (nested_ > 0)
        {
            // Inside callGuest no other thread may run: only a wait that is already over can finish
            auto       result   = WaitResult{};
            auto       finished = block->wait.objects.empty() && block->wait.timeoutMs == 0;
            const auto waiter   = thread.id;
            for (auto index = std::size_t{}; index < block->wait.objects.size() && !finished; ++index)
            {
                if (block->wait.objects[index]->signaled(waiter))
                {
                    block->wait.objects[index]->acquire(waiter);
                    result.index = static_cast<std::uint32_t>(index);
                    finished     = true;
                }
            }
            if (!finished && block->wait.timeoutMs == 0)
            {
                result.timedOut = true;
                finished        = true;
            }
            if (!finished)
            {
                halt("A guest callback blocked inside callGuest");
                return false;
            }
            complete(context, trap, block->resume(result));
            return !halted_;
        }
        auto* const waiter = &thread;
        scheduler_.block(std::move(block->wait), [this, waiter, trap, resume = std::move(block->resume)](const WaitResult& result)
                         {
                             complete(waiter->context, trap, resume(result));
                         });
        return true;
    }
    if (const auto* const stop = std::get_if<Halt>(&outcome))
    {
        halt(stop->reason);
        return false;
    }
    return true; // Resume: the handler set the context itself
}

void Machine::complete(Context& context, const std::uint32_t trap, const Return& result)
{
    const auto* const function = boundary_.find(trap);
    if (!function)
    {
        halt(std::format("No stand-in behind the trap at {:#x}", trap));
        return;
    }
    const auto back = memory_.read<std::uint32_t>(context.r[kEsp]);
    context.r[kEsp] += 4;
    if (function->convention != Convention::Cdecl)
    {
        context.r[kEsp] += 4u * function->argc;
    }
    context.r[kEax] = result.eax;
    context.r[kEdx] = result.edx;
    context.eip     = back;
}

auto Machine::exception(Thread& thread, const Event& event) -> bool
{
    auto& context = thread.context;
    if (event.stop == Stop::Fault)
    {
        const auto info = std::array<std::uint32_t, 2>{ accessKind(event.fault.access), event.fault.address };
        ++exceptions_;
        if (!raise(kAccessViolation, event.address, info))
        {
            halt(std::format("Unhandled access violation at {:#x} {} {:#x}", event.address, accessName(event.fault.access), event.fault.address));
            return false;
        }
        return !halted_;
    }
    // Interrupts: what Windows reports, with eip where Windows leaves it in the CONTEXT
    auto code    = std::uint32_t{ kAccessViolation };
    auto address = event.address;
    auto info    = std::array<std::uint32_t, 2>{ 0, 0xFFFFFFFF };
    auto count   = std::size_t{};
    switch (event.vector)
    {
        case 0:
            code = kIntegerDivide;
            break;
        case 1:
            // A single step: the trap flag is cleared, eip is the next instruction
            code = kSingleStep;
            context.eflags &= ~kTF;
            address = context.eip;
            break;
        case 3:
            // INT3: the CONTEXT's eip points back at the breakpoint byte
            code        = kBreakpoint;
            context.eip = context.eip - 1;
            address     = context.eip;
            break;
        case 4:
            code    = kIntegerOverflow;
            address = context.eip;
            break;
        case 5:
            code = kArrayBounds;
            break;
        case 6:
            code = kIllegalInstruction;
            break;
        case 13:
            code = kPrivileged;
            break;
        case 0x2D:
            // The kernel debugger service: a breakpoint when no debugger is attached
            code    = kBreakpoint;
            address = context.eip;
            break;
        default:
            // INT n with no user gate: a general protection fault at the instruction
            context.eip = event.address;
            count       = 2;
            break;
    }
    ++exceptions_;
    if (!raise(code, address, std::span{ info.data(), count }))
    {
        halt(std::format("Unhandled exception {:#010x} at {:#x}", code, event.address));
        return false;
    }
    return !halted_;
}

auto Machine::callGuest(const std::uint32_t function, const std::span<const std::uint32_t> args, const Convention convention) -> std::uint32_t
{
    auto&      thread   = scheduler_.current();
    auto&      context  = thread.context;
    const auto savedEip = context.eip;
    const auto savedEsp = context.r[kEsp];
    auto       stack    = args;
    if (convention == Convention::Thiscall && !args.empty())
    {
        context.r[kEcx] = args.front();
        stack           = args.subspan(1);
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    {
        context.r[kEsp] -= 4;
        memory_.write<std::uint32_t>(context.r[kEsp], *it);
    }
    context.r[kEsp] -= 4;
    memory_.write<std::uint32_t>(context.r[kEsp], boundary_.returnTrap());
    context.eip = function;
    ++nested_;
    while (!halted_)
    {
        const auto event = cpu_.run(context, kSlice);
        if (event.stop == Stop::Trap && event.address == boundary_.returnTrap())
        {
            break;
        }
        if (!handle(thread, event))
        {
            break;
        }
    }
    --nested_;
    const auto result = context.r[kEax];
    context.r[kEsp]   = savedEsp;
    context.eip       = savedEip;
    return result;
}

auto Machine::raise(std::uint32_t, std::uint32_t, std::span<const std::uint32_t>) -> bool
{
    return false;
}

void Machine::halt(std::string reason)
{
    if (!halted_)
    {
        halted_ = true;
        reason_ = std::move(reason);
    }
}

} // namespace tmweb
