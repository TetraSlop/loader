#include "core/threads.h"

#include <chrono>
#include <stdexcept>

namespace tmweb
{

void Waitable::acquire(ThreadId)
{
}

Scheduler::Scheduler(Memory& memory)
: memory_(memory)
{
    const auto start = std::chrono::steady_clock::now();
    clock_           = [start]()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    };
}

auto Scheduler::create(std::uint32_t, std::uint32_t, std::uint32_t, bool, std::uint32_t) -> Thread&
{
    throw std::logic_error("Scheduler::create is not implemented yet");
}

auto Scheduler::createMain(std::uint32_t) -> Thread&
{
    throw std::logic_error("Scheduler::createMain is not implemented yet");
}

auto Scheduler::current() -> Thread&
{
    return *current_;
}

auto Scheduler::find(const ThreadId id) -> Thread*
{
    for (auto& thread : threads_)
    {
        if (thread->id == id)
        {
            return thread.get();
        }
    }
    return nullptr;
}

auto Scheduler::threads() -> std::vector<std::unique_ptr<Thread>>&
{
    return threads_;
}

void Scheduler::suspend(Thread& thread)
{
    ++thread.suspendCount;
    thread.state = ThreadState::Suspended;
}

void Scheduler::resume(Thread& thread)
{
    if (thread.suspendCount > 0 && --thread.suspendCount == 0 && thread.state == ThreadState::Suspended)
    {
        thread.state = ThreadState::Ready;
    }
}

void Scheduler::exit(Thread& thread, const std::uint32_t code)
{
    thread.state    = ThreadState::Exited;
    thread.exitCode = code;
}

void Scheduler::block(Wait, std::function<void(const WaitResult&)>)
{
    throw std::logic_error("Scheduler::block is not implemented yet");
}

auto Scheduler::schedule() -> bool
{
    return current_ != nullptr;
}

auto Scheduler::now() const -> std::uint64_t
{
    return clock_();
}

void Scheduler::setClock(std::function<std::uint64_t()> clock)
{
    clock_ = std::move(clock);
}

} // namespace tmweb
