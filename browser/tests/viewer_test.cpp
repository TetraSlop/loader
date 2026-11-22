// The real Viewer (hle/viewer.h) on the user's polcore_native.dll. Needs TMWEB_RUNTIME, the runtime folder; skipped
// without it. The Game IRC login needs a server and runs under Node instead (browser/README.md, "The real Viewer").
#include "hle/boot.h"
#include "tests/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>

using namespace tmweb;
using namespace tmweb::hle;

namespace
{

auto runtime() -> std::string
{
    const auto* value = std::getenv("TMWEB_RUNTIME");
    return value ? value : "";
}

// The key generation's routines and their stand-ins (hle/viewer.cpp): RVA, stand-in, argument count
struct Routine
{
    std::uint32_t rva;
    const char*   standIn;
    unsigned      argc;
};

constexpr auto kRoutines = std::array{ Routine{ 0x2C1D0, "viewer.montgomery", 4 }, Routine{ 0x2CA20, "viewer.add512", 2 }, Routine{ 0x2CFA0, "viewer.shift_right512", 1 } };

constexpr std::uint32_t kNumber = 64; // bytes in one of polcore's 512-bit numbers

} // namespace

TEST_CASE("TM_native.dll boots to Direct3DCreate8(220) on the real polcore")
{
    const auto folder = runtime();
    if (folder.empty())
    {
        SKIP("TMWEB_RUNTIME is not set");
    }
    auto       machine = Machine{};
    const auto boot    = Boot::start(machine, std::make_shared<DiskFiles>(folder), BootOptions{ .realViewer = true });
    auto       steps   = std::vector<Step>{};
    boot->onStep       = [&](const Step step)
    {
        steps.push_back(step);
    };
    auto       engine = test::Engine{ machine };
    const auto result = engine.run(test::Engine::kBudget);
    INFO(result.reason);
    REQUIRE(result.reason.starts_with("unhandled import d3d8.dll!Direct3DCreate8 from "));
    CHECK(machine.memory().read<std::uint32_t>(machine.scheduler().current().context.r[kEsp] + 4) == 220);
    CHECK(steps == std::vector<Step>{ Step::DllMain, Step::GetClassObject, Step::CreateInstance, Step::Viewer });

    auto& p = boot->process();
    CHECK(p.viewer.base == layout::kViewerImage);
    // GameStart got polcore's core object, so the stand-in Viewer saw nothing
    CHECK(boot->viewer().core.empty());
    CHECK(boot->viewer().table.empty());
    // The boot's option tickets read the user's files in polcore's worker threads
    const auto opened = [&](const std::string_view name)
    {
        return std::ranges::any_of(p.trace, [&](const std::string& line)
                                   {
                                       return line.starts_with("CreateFileA ") && line.contains(name);
                                   });
    };
    CHECK(opened("env.dat"));
    CHECK(opened("option.bin"));
}

// Inputs recorded from the real key generation (with the stand-ins in place, so it is quick), then each routine run
// on them twice in a second machine: polcore's own code and the stand-in, compared word for word
TEST_CASE("The RSA key generation's stand-ins match polcore's routines on recorded inputs")
{
    const auto folder = runtime();
    if (folder.empty())
    {
        SKIP("TMWEB_RUNTIME is not set");
    }

    struct Recorded
    {
        unsigned                               routine;
        std::vector<std::uint32_t>             args;
        std::vector<std::vector<std::uint8_t>> numbers;
    };

    auto recorded = std::vector<Recorded>{};
    auto calls    = std::array<unsigned, kRoutines.size()>{};
    {
        auto  machine = Machine{};
        auto& memory  = machine.memory();
        machine.boundary().observe(
            [&](const Call& call)
            {
                if (call.name() == "ws2_32.dll!#115") // WSAStartup, imported by ordinal
                {
                    call.machine().halt("past the key generation");
                }
                for (auto index = 0u; index < kRoutines.size(); ++index)
                {
                    if (call.name() != std::format("tmweb!{}", kRoutines[index].standIn) || (++calls[index] > 200 && calls[index] % 97 != 0))
                    {
                        continue;
                    }
                    auto record = Recorded{ index, {}, {} };
                    for (auto arg = 0u; arg < kRoutines[index].argc; ++arg)
                    {
                        record.args.push_back(call.arg(arg));
                        auto number = std::vector<std::uint8_t>(kNumber);
                        memory.read(call.arg(arg), number);
                        record.numbers.push_back(std::move(number));
                    }
                    recorded.push_back(std::move(record));
                }
            });
        Boot::start(machine, std::make_shared<DiskFiles>(folder), BootOptions{ .realViewer = true, .probe = true });
        auto       engine = test::Engine{ machine };
        const auto result = engine.run(test::Engine::kBudget);
        INFO(result.reason);
        REQUIRE(result.reason == "past the key generation");
    }
    INFO(std::format("{} Montgomery multiplications, {} additions, {} shifts", calls[0], calls[1], calls[2]));
    REQUIRE(calls[0] > 1000);

    // A machine with polcore unpacked and nothing patched: DllMain only
    auto  machine = Machine{};
    auto& memory  = machine.memory();
    auto  files   = DiskFiles{ folder };
    auto  p       = Process{ machine, files, kInstallRoot };
    install(p);
    auto viewer = Viewer{ p, {}, false };
    viewer.install();
    machine.boundary().publish();
    finishInstall(p);
    viewer.load(*files.read(p.viewerFile));
    machine.scheduler().createMain(0x100000);
    auto& context   = machine.scheduler().current().context;
    context.r[kEsp] = machine.scheduler().current().stackBase - 0x100;
    machine.cpu().setTraps(machine.boundary().traps()); // run() does this; callGuest alone does not
    REQUIRE(machine.callGuest(p.viewer.entry, std::array{ p.viewer.base, 1u, 0u }, Convention::Stdcall) == 1);

    // Each recorded call: the same numbers at the same relative layout, once per implementation
    const auto scratch = p.allocate(16 * kNumber);
    auto       checked = std::array<unsigned, kRoutines.size()>{};
    const auto run     = [&](const Recorded& record, const std::uint32_t function)
    {
        // Aliased arguments stay aliased: equal addresses map to the same scratch number
        auto args = std::vector<std::uint32_t>{};
        for (auto arg = 0u; arg < record.args.size(); ++arg)
        {
            const auto first = static_cast<std::uint32_t>(std::ranges::find(record.args, record.args[arg]) - record.args.begin());
            args.push_back(scratch + first * kNumber);
            memory.write(args.back(), std::span<const std::uint8_t>{ record.numbers[first] });
        }
        const auto eax   = machine.callGuest(function, args, Convention::Cdecl);
        auto       after = std::vector<std::uint8_t>(record.args.size() * kNumber);
        memory.read(scratch, after);
        return std::pair{ eax, after };
    };
    for (const auto& record : recorded)
    {
        const auto& routine  = kRoutines[record.routine];
        const auto  original = run(record, p.viewer.base + routine.rva);
        const auto  standIn  = run(record, machine.boundary().resolve("tmweb", routine.standIn));
        INFO(std::format("{} call with arguments {}", routine.standIn, record.args.size()));
        CHECK(standIn.first == original.first);
        CHECK(standIn.second == original.second);
        ++checked[record.routine];
    }
    // The additions and shifts run inside the Montgomery stand-in, so check them on its recorded numbers too
    for (const auto& record : recorded)
    {
        if (record.routine != 0)
        {
            continue;
        }
        for (const auto routine : { 1u, 2u })
        {
            auto derived = Recorded{ routine, {}, {} };
            for (auto arg = 0u; arg < kRoutines[routine].argc; ++arg)
            {
                derived.args.push_back(0x1000 * (arg + 1));
                derived.numbers.push_back(record.numbers[3 - arg]); // r, then b
            }
            const auto original = run(derived, p.viewer.base + kRoutines[routine].rva);
            const auto standIn  = run(derived, machine.boundary().resolve("tmweb", kRoutines[routine].standIn));
            CHECK(standIn == original);
            ++checked[routine];
        }
    }
    CHECK(checked[0] > 200);
    CHECK(checked[1] > 200);
    CHECK(checked[2] > 200);
}
