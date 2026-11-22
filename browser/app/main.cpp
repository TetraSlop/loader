// Command-line runner and benchmark, native or under Node (NODERAWFS):
//   tmweb <runtime folder> [--budget N] [--frames N] [--profile]
// Boots the folder and runs as a host would (through idle time) until a halt, the budget or N frames, then prints
// what stopped it, the instructions retired, the frames presented and the speed. --profile samples the guest's
// eip every 20,000 instructions and prints where the time went, by thread and 64-byte block.
#include "app/boot.h"
#include "core/machine.h"
#if defined(__EMSCRIPTEN__)
#include "com/d3dx.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

auto main(const int argc, char** argv) -> int
{
    if (argc < 2)
    {
        std::printf("usage: tmweb <runtime folder> [--budget N] [--frames N] [--profile]\n");
        return 2;
    }
    auto budget    = std::uint64_t{ 20'000'000'000 };
    auto maxFrames = std::uint64_t{ 0 };
    auto profile   = false;
    for (auto index = 2; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--budget") == 0 && index + 1 < argc)
        {
            budget = std::strtoull(argv[++index], nullptr, 10);
        }
        else if (std::strcmp(argv[index], "--frames") == 0 && index + 1 < argc)
        {
            maxFrames = std::strtoull(argv[++index], nullptr, 10);
        }
        else if (std::strcmp(argv[index], "--profile") == 0)
        {
            profile = true;
        }
#if defined(__EMSCRIPTEN__)
        else if (std::strcmp(argv[index], "--verify-d3dx") == 0)
        {
            tmweb::verifyD3dx(true);
        }
#endif
    }
    auto machine = tmweb::Machine{};
    tmweb::boot(machine, argv[1]);
    // Frames are counted at Present, whoever implements it
    auto presents = std::uint64_t{};
    auto firstAt  = std::uint64_t{};
    machine.boundary().observe(
        [&](const tmweb::Call& call)
        {
            if (call.name().ends_with("!Present"))
            {
                if (presents == 0)
                {
                    firstAt = machine.cpu().retired();
                }
                ++presents;
            }
        });
    auto samples = std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t>{};

    const auto started = std::chrono::steady_clock::now();
    auto       idle    = std::chrono::steady_clock::duration{};
    auto       total   = std::uint64_t{};
    auto       reason  = std::string{ "budget" };
    const auto slice   = std::uint64_t{ profile ? 20'000u : 10'000'000u };
    while (total < budget && (maxFrames == 0 || presents < maxFrames))
    {
        const auto result = machine.run(std::min(budget - total, slice));
        total += result.instructions;
        if (profile && presents > 0)
        {
            const auto& thread = machine.scheduler().current();
            ++samples[{ thread.id, thread.context.eip & ~63u }];
        }
        if (result.halted)
        {
            reason = result.reason;
            break;
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
    std::printf("tmweb: %llu instructions, %llu frames, %.2f s (%.2f s busy): %.1f MIPS\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(presents), elapsed, busy, static_cast<double>(total) / busy / 1e6);
    if (presents > 1)
    {
        std::printf("tmweb: first frame after %llu instructions; %.0f instructions per frame since\n", static_cast<unsigned long long>(firstAt), static_cast<double>(machine.cpu().retired() - firstAt) / static_cast<double>(presents - 1));
    }
    if (profile)
    {
        auto sorted = std::vector<std::pair<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>>{};
        auto count  = std::uint64_t{};
        for (const auto& [key, hits] : samples)
        {
            sorted.emplace_back(hits, key);
            count += hits;
        }
        std::ranges::sort(sorted, std::greater{});
        for (auto index = std::size_t{}; index < std::min<std::size_t>(sorted.size(), 25); ++index)
        {
            std::printf("  %5.1f%%  thread %u  eip %#010x\n", 100.0 * static_cast<double>(sorted[index].first) / static_cast<double>(count), sorted[index].second.first, sorted[index].second.second);
        }
    }
#if defined(__EMSCRIPTEN__)
    std::printf("%s", tmweb::d3dxReport().c_str());
#endif
    return 0;
}
