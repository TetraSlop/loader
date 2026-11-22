// Command-line runner and benchmark, native or under Node (NODERAWFS):
//   tmweb <runtime folder> [--budget N]
// Boots the folder, runs as a host would (through idle time, frame by frame) until a halt or the budget, then
// prints what stopped it, the instructions retired, the frames presented and the speed.
#include "app/boot.h"
#include "core/machine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

auto main(const int argc, char** argv) -> int
{
    if (argc < 2)
    {
        std::printf("usage: tmweb <runtime folder> [--budget N]\n");
        return 2;
    }
    auto budget = std::uint64_t{ 20'000'000'000 };
    for (auto index = 2; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], "--budget") == 0)
        {
            budget = std::strtoull(argv[index + 1], nullptr, 10);
        }
    }
    auto machine = tmweb::Machine{};
    tmweb::boot(machine, argv[1]);
    const auto started = std::chrono::steady_clock::now();
    auto       idle    = std::chrono::steady_clock::duration{};
    auto       total   = std::uint64_t{};
    auto       frames  = std::uint64_t{};
    auto       reason  = std::string{ "budget" };
    while (total < budget)
    {
        const auto result = machine.run(std::min<std::uint64_t>(budget - total, 10'000'000));
        total += result.instructions;
        if (result.halted)
        {
            reason = result.reason;
            break;
        }
        if (result.yielded)
        {
            ++frames;
        }
        if (result.idle)
        {
            if (!result.nextDeadlineMs)
            {
                reason = "idle with nothing to wait for";
                break;
            }
            const auto before = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
            idle += std::chrono::steady_clock::now() - before;
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto busy    = elapsed - std::chrono::duration<double>(idle).count();
    std::printf("tmweb: %s\n", reason.c_str());
    std::printf("tmweb: %llu instructions, %llu frames, %.2f s (%.2f s busy): %.1f MIPS\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(frames), elapsed, busy, static_cast<double>(total) / busy / 1e6);
    return 0;
}
