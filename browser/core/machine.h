#pragma once

// One emulated process: guest memory, the CPU, threads and the stand-in boundary. run() is the main loop: it runs
// the current thread, dispatches traps to stand-ins, turns faults and interrupts into Windows structured exceptions
// on the thread's fs:[0] chain, and switches threads when one blocks.

#include "core/boundary.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "core/threads.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace tmweb
{

// Exception codes the machine raises
enum ExceptionCode : std::uint32_t
{
    kAccessViolation    = 0xC0000005,
    kBreakpoint         = 0x80000003,
    kSingleStep         = 0x80000004,
    kIllegalInstruction = 0xC000001D,
    kIntegerDivide      = 0xC0000094,
    kIntegerOverflow    = 0xC0000095,
    kPrivileged         = 0xC0000096,
};

struct RunResult
{
    bool          halted = false; // a stand-in halted, an exception went unhandled, or nothing can run
    std::string   reason;
    std::uint64_t instructions = 0;
    std::uint32_t exceptions   = 0; // structured exceptions dispatched
    // Not halted, but every thread waits on a host event (socket data, input) or a timeout: the host goes back to its
    // event loop before running again. nextDeadlineMs is the earliest timeout, on the scheduler's clock.
    bool                         idle = false;
    std::optional<std::uint32_t> nextDeadlineMs;
    // yield() was called: the machine stopped at an instruction boundary so the host can show a frame
    bool yielded = false;
};

class Machine final
{
public:
    Machine();

    auto memory() -> Memory&;
    auto cpu() -> Cpu&;
    auto boundary() -> Boundary&;
    auto scheduler() -> Scheduler&;

    // Run at most budget instructions, summed over threads; returns early on a halt or when idle
    auto run(std::uint64_t budget) -> RunResult;

    // Call guest code from a stand-in (window procedures, hook procedures, callbacks) on the current thread and
    // return its EAX. Runs nested: other threads do not run until it returns, and the callee must not block.
    auto callGuest(std::uint32_t function, std::span<const std::uint32_t> args, Convention convention)
        -> std::uint32_t;

    // Raise a structured exception on the current thread as if at eip: builds EXCEPTION_RECORD and CONTEXT on its
    // stack and enters the first fs:[0] handler. False when the chain is empty.
    auto raise(std::uint32_t code, std::uint32_t address, std::span<const std::uint32_t> information) -> bool;

    // Halt from inside a handler or the host; run() returns with the reason
    void halt(std::string reason);

    // Make run() return at the next instruction boundary with RunResult::yielded, neither halted nor idle. A stand-in
    // calls it after Present, so each host task shows one frame.
    void yield();

    // Finish the stand-in call stopped at trap: EAX/EDX, the return address popped, and for stdcall and thiscall
    // the arguments too. The machine does this for a Return at once and for a Block when it wakes.
    void complete(Context& context, std::uint32_t trap, const Return& result);

private:
    // Act on why the CPU stopped; false when the machine halted
    auto handle(Thread& thread, const Event& event) -> bool;
    auto dispatchTrap(Thread& thread, std::uint32_t trap) -> bool;
    auto exception(Thread& thread, const Event& event) -> bool;

    Memory        memory_;
    Cpu           cpu_;
    Boundary      boundary_;
    Scheduler     scheduler_;
    bool          halted_ = false;
    std::string   reason_;
    unsigned      nested_     = 0; // callGuest depth
    bool          yield_      = false;
    std::uint32_t exceptions_ = 0;
};

} // namespace tmweb
