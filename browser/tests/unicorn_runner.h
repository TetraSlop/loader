#pragma once

// Runs guest code on Unicorn instead of the interpreter, so the stand-ins can be tested against real guest code
// before the interpreter exists. Test-only, behind the TMWEB_UNICORN option.
//
// Unicorn shares tmweb's guest memory: a page is mapped into Unicorn with uc_mem_map_ptr the first time the guest
// touches it, so stand-ins that read or write through Memory and guest code see the same bytes.

#include "core/machine.h"

#include <cstdint>
#include <vector>

struct uc_struct;

namespace tmweb::test
{

class UnicornRunner final
{
public:
    explicit UnicornRunner(Machine& machine);
    ~UnicornRunner();
    UnicornRunner(const UnicornRunner&)                    = delete;
    UnicornRunner(UnicornRunner&&)                         = delete;
    auto operator=(const UnicornRunner&) -> UnicornRunner& = delete;
    auto operator=(UnicornRunner&&) -> UnicornRunner&      = delete;

    // Cpu::run's contract, except that the budget counts basic blocks
    auto run(Context& context, std::uint64_t budget) -> Event;

    // Machine::run's loop on the current thread: traps go to their stand-ins, faults and interrupts become
    // structured exceptions. One thread only: a Block on a plain sleep returns at once, any other Block halts.
    auto runUntilHalt(std::uint64_t budget) -> RunResult;

private:
    friend struct Hooks;

    Machine&          machine_;
    uc_struct*        uc_ = nullptr;
    std::vector<bool> shared_; // per 4 KiB page: mapped into Unicorn
    std::uint32_t     fsBase_ = 0;
    std::uint64_t     blocks_ = 0;
    std::uint64_t     limit_  = 0;
    Event             event_{};
    bool              stopped_ = false;
};

} // namespace tmweb::test
