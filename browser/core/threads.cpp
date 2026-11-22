#include "core/threads.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace tmweb
{
namespace
{

constexpr std::uint64_t kNoDeadline = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t kProcessId  = 0x100;

enum TebField : std::uint32_t
{
    kTebExceptionList = 0x00,
    kTebStackBase     = 0x04,
    kTebStackLimit    = 0x08,
    kTebSelf          = 0x18,
    kTebProcessId     = 0x20,
    kTebThreadId      = 0x24,
    kTebPeb           = 0x30,
};

// A thread's handle: signaled once it has exited
class ThreadObject final : public Waitable
{
public:
    explicit ThreadObject(const Thread& thread)
    : thread_(thread)
    {
    }

    auto signaled(ThreadId) const -> bool override
    {
        return thread_.state == ThreadState::Exited;
    }

private:
    const Thread& thread_;
};

auto roundUp(const std::uint32_t value, const std::uint32_t alignment) -> std::uint32_t
{
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

void Waitable::acquire(ThreadId)
{
}

Scheduler::Scheduler(Memory& memory)
: memory_(memory)
{
    const auto start = std::chrono::steady_clock::now();
    clock_           = [start]()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    };
}

auto Scheduler::newThread(const std::uint32_t stackSize) -> Thread&
{
    const auto size  = roundUp(std::max(stackSize, 0x10000u), 0x10000);
    const auto teb   = nextTeb_;
    const auto limit = teb + kPageSize;
    const auto base  = limit + size;
    if (static_cast<std::uint64_t>(base) + kPageSize > layout::kThreadsEnd)
    {
        throw std::runtime_error("No room for another guest thread");
    }
    // An unmapped guard page follows each stack
    nextTeb_ = base + kPageSize;
    memory_.map(teb, kPageSize + size);

    auto thread             = std::make_unique<Thread>();
    thread->id              = nextId_++;
    thread->teb             = teb;
    thread->stackBase       = base;
    thread->stackLimit      = limit;
    thread->handle          = std::make_shared<ThreadObject>(*thread);
    thread->context.fsBase  = teb;
    thread->context.r[kEsp] = base;

    memory_.write<std::uint32_t>(teb + kTebExceptionList, 0xFFFFFFFF);
    memory_.write<std::uint32_t>(teb + kTebStackBase, base);
    memory_.write<std::uint32_t>(teb + kTebStackLimit, limit);
    memory_.write<std::uint32_t>(teb + kTebSelf, teb);
    memory_.write<std::uint32_t>(teb + kTebProcessId, kProcessId);
    memory_.write<std::uint32_t>(teb + kTebThreadId, thread->id);
    if (!threads_.empty())
    {
        memory_.write<std::uint32_t>(teb + kTebPeb, memory_.read<std::uint32_t>(threads_.front()->teb + kTebPeb));
    }
    threads_.push_back(std::move(thread));
    return *threads_.back();
}

auto Scheduler::create(const std::uint32_t start, const std::uint32_t parameter, const std::uint32_t stackSize, const bool suspended, const std::uint32_t exitTrap) -> Thread&
{
    auto& thread = newThread(stackSize);
    auto& esp    = thread.context.r[kEsp];
    esp -= 4;
    memory_.write<std::uint32_t>(esp, parameter);
    esp -= 4;
    memory_.write<std::uint32_t>(esp, exitTrap);
    thread.context.eip = start;
    if (suspended)
    {
        thread.suspendCount = 1;
        thread.state        = ThreadState::Suspended;
    }
    if (!current_)
    {
        current_ = &thread;
    }
    return thread;
}

auto Scheduler::createMain(const std::uint32_t stackSize) -> Thread&
{
    auto& thread = newThread(stackSize);
    current_     = &thread;
    return thread;
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
    if (thread.state == ThreadState::Ready)
    {
        thread.state = ThreadState::Suspended;
    }
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
    for (const auto& listener : exitListeners_)
    {
        listener(thread);
    }
    thread.state    = ThreadState::Exited;
    thread.exitCode = code;
    std::erase_if(parked_, [&thread](const Parked& parked)
                  {
                      return parked.thread == &thread;
                  });
}

void Scheduler::onExit(std::function<void(Thread&)> listener)
{
    exitListeners_.push_back(std::move(listener));
}

void Scheduler::block(Wait wait, std::function<void(const WaitResult&)> onWake)
{
    auto deadline = kNoDeadline;
    if (wait.timeoutMs != kInfinite)
    {
        deadline = now() + wait.timeoutMs;
    }
    current_->state = ThreadState::Waiting;
    parked_.push_back(Parked{ current_, std::move(wait), deadline, std::move(onWake) });
}

void Scheduler::wake()
{
    const auto time = now();
    // Collect first, then call back: a callback may park another wait
    auto woken = std::vector<std::pair<Parked, WaitResult>>{};
    for (auto it = parked_.begin(); it != parked_.end();)
    {
        auto&      wait   = it->wait;
        const auto waiter = it->thread->id;
        auto       done   = false;
        auto       result = WaitResult{};
        if (!wait.objects.empty())
        {
            if (wait.all)
            {
                done = std::ranges::all_of(wait.objects, [waiter](const auto& object)
                                           {
                                               return object->signaled(waiter);
                                           });
                if (done)
                {
                    for (const auto& object : wait.objects)
                    {
                        object->acquire(waiter);
                    }
                }
            }
            else
            {
                for (auto index = std::size_t{}; index < wait.objects.size(); ++index)
                {
                    if (wait.objects[index]->signaled(waiter))
                    {
                        wait.objects[index]->acquire(waiter);
                        result.index = static_cast<std::uint32_t>(index);
                        done         = true;
                        break;
                    }
                }
            }
        }
        if (!done && time >= it->deadline)
        {
            result.timedOut = true;
            done            = true;
        }
        if (done)
        {
            it->thread->state = ThreadState::Ready;
            if (it->thread->suspendCount > 0)
            {
                it->thread->state = ThreadState::Suspended;
            }
            woken.emplace_back(std::move(*it), result);
            it = parked_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto& [parked, result] : woken)
    {
        parked.onWake(result);
    }
}

auto Scheduler::schedule() -> bool
{
    wake();
    if (threads_.empty())
    {
        return false;
    }
    // Round robin, starting after the current thread and coming back to it last
    auto start = std::size_t{};
    for (auto index = std::size_t{}; index < threads_.size(); ++index)
    {
        if (threads_[index].get() == current_)
        {
            start = index;
        }
    }
    for (auto step = std::size_t{ 1 }; step <= threads_.size(); ++step)
    {
        auto* const candidate = threads_[(start + step) % threads_.size()].get();
        if (candidate->state == ThreadState::Ready)
        {
            current_ = candidate;
            return true;
        }
    }
    return false;
}

auto Scheduler::nextDeadline() const -> std::optional<std::uint64_t>
{
    auto earliest = kNoDeadline;
    for (const auto& parked : parked_)
    {
        earliest = std::min(earliest, parked.deadline);
    }
    if (earliest == kNoDeadline)
    {
        return std::nullopt;
    }
    return earliest;
}

auto Scheduler::waiting() const -> std::size_t
{
    return parked_.size();
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
