#pragma once

// Guest threads. They run one at a time on the host thread: the machine gives the current thread an instruction
// slice, and switches when a stand-in blocks (WaitForSingleObject, Sleep, a socket with no data), the slice runs
// out, or the thread exits. Stand-ins see threads only through this interface: kernel objects that can be waited
// on implement Waitable, and a handler that must wait returns a Block (core/boundary.h).

#include "core/cpu.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace tmweb
{

using ThreadId = std::uint32_t;

enum : std::uint32_t
{
    kInfinite = 0xFFFFFFFF,
};

// Something a thread can wait on: an event, mutex, semaphore, thread or socket readiness
class Waitable
{
public:
    Waitable()                                   = default;
    Waitable(const Waitable&)                    = delete;
    Waitable(Waitable&&)                         = delete;
    auto operator=(const Waitable&) -> Waitable& = delete;
    auto operator=(Waitable&&) -> Waitable&      = delete;
    virtual ~Waitable()                          = default;

    virtual auto signaled(ThreadId waiter) const -> bool = 0;
    // The wait succeeded: auto-reset events reset, semaphores count down, mutexes take an owner
    virtual void acquire(ThreadId waiter);
};

struct Wait
{
    std::vector<std::shared_ptr<Waitable>> objects; // none: a plain sleep for timeoutMs
    bool                                   all       = false;
    std::uint32_t                          timeoutMs = kInfinite;
};

struct WaitResult
{
    bool          timedOut = false;
    std::uint32_t index    = 0; // which object, for a wait on any
};

enum class ThreadState : std::uint8_t
{
    Ready,
    Waiting,
    Suspended,
    Exited,
};

struct Thread
{
    ThreadId                  id = 0;
    Context                   context{};
    ThreadState               state        = ThreadState::Ready;
    std::uint32_t             suspendCount = 0;
    std::uint32_t             exitCode     = 0;
    std::uint32_t             teb          = 0; // FS base: SEH chain at +0, TLS slots at +0xE10, last error at +0x34
    std::uint32_t             stackBase    = 0; // highest address
    std::uint32_t             stackLimit   = 0;
    std::shared_ptr<Waitable> handle; // signaled once the thread exits
};

class Scheduler final
{
public:
    explicit Scheduler(Memory& memory);

    // A new thread whose first instruction is start(parameter), returning into ExitThread's stand-in
    auto create(std::uint32_t start, std::uint32_t parameter, std::uint32_t stackSize, bool suspended, std::uint32_t exitTrap) -> Thread&;
    // The thread the machine starts from: no start routine, the caller sets its context
    auto createMain(std::uint32_t stackSize) -> Thread&;
    // What create and createMain share: a thread with a mapped stack and a TEB (SEH chain end, stack bounds, self,
    // client ids, the PEB of the first thread), FS on the TEB and ESP at the stack top
    auto newThread(std::uint32_t stackSize) -> Thread&;

    auto current() -> Thread&;
    auto find(ThreadId id) -> Thread*;
    auto threads() -> std::vector<std::unique_ptr<Thread>>&;

    void suspend(Thread& thread);
    void resume(Thread& thread);
    void exit(Thread& thread, std::uint32_t code);

    // Park the current thread on a wait; the machine calls onWake when it is satisfied or times out
    void block(Wait wait, std::function<void(const WaitResult&)> onWake);
    // Pick the next thread to run, waking satisfied waits; false when every thread is parked or gone and no
    // timeout is pending
    auto schedule() -> bool;
    // The earliest timeout among parked threads, on the scheduler's clock
    auto nextDeadline() const -> std::optional<std::uint64_t>;
    // Threads parked on a wait
    auto waiting() const -> std::size_t;

    // Milliseconds since start for GetTickCount and timeouts. The host clock by default; tests set their own.
    auto now() const -> std::uint64_t;
    void setClock(std::function<std::uint64_t()> clock);

private:
    void wake();

    struct Parked
    {
        Thread*                                thread = nullptr;
        Wait                                   wait;
        std::uint64_t                          deadline = 0;
        std::function<void(const WaitResult&)> onWake;
    };

    Memory&                              memory_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::vector<Parked>                  parked_;
    Thread*                              current_ = nullptr;
    ThreadId                             nextId_  = 1;
    std::uint32_t                        nextTeb_ = layout::kThreads;
    std::function<std::uint64_t()>       clock_;
};

} // namespace tmweb
