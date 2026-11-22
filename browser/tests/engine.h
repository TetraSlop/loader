#pragma once

#include <chrono>
#include <thread>

// What runs guest code in a test: the interpreter through Machine::run, or Unicorn when the file is built into
// tmweb_unicorn_tests (TMWEB_ON_UNICORN)

#include "core/machine.h"

#ifdef TMWEB_ON_UNICORN
#include "tests/unicorn_runner.h"
#define TMWEB_ENGINE " (Unicorn)"
#else
#define TMWEB_ENGINE ""
#endif

namespace tmweb::test
{

#ifdef TMWEB_ON_UNICORN
class Engine final
{
public:
    static constexpr std::uint64_t kBudget = 200'000'000; // basic blocks

    explicit Engine(Machine& machine)
    : runner_(machine)
    {
    }

    auto run(const std::uint64_t budget) -> RunResult
    {
        return runner_.runUntilHalt(budget);
    }

    // Run a context without dispatching, until a trap or the budget
    auto probe(Context& context, const std::uint64_t budget) -> Event
    {
        return runner_.run(context, budget);
    }

private:
    UnicornRunner runner_;
};
#else
class Engine final
{
public:
    static constexpr std::uint64_t kBudget = 4'000'000'000; // instructions

    explicit Engine(Machine& machine)
    : machine_(machine)
    {
    }

    // Run until a halt or the budget, as a host would: when every thread waits on a timeout, let the clock reach it
    auto run(const std::uint64_t budget) -> RunResult
    {
        auto total = RunResult{};
        while (total.instructions < budget)
        {
            const auto result = machine_.run(budget - total.instructions);
            total.instructions += result.instructions;
            total.exceptions += result.exceptions;
            if (result.halted)
            {
                total.halted = true;
                total.reason = result.reason;
                return total;
            }
            if (result.idle)
            {
                if (!result.nextDeadlineMs)
                {
                    total.halted = true;
                    total.reason = "every guest thread waits on something nobody will signal";
                    return total;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
            }
        }
        return total;
    }

    auto probe(Context& context, const std::uint64_t budget) -> Event
    {
        machine_.cpu().setTraps(machine_.boundary().traps());
        return machine_.cpu().run(context, budget);
    }

private:
    Machine& machine_;
};
#endif

} // namespace tmweb::test
