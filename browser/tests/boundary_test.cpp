#include "core/machine.h"
#include "hle/image.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

using namespace tmweb;
using tmweb::hle::findExport;

namespace
{

// Walk an export directory the way a protector does, by name
auto exportByName(Memory& memory, const std::uint32_t base, const std::string& name) -> std::uint32_t
{
    REQUIRE(memory.read<std::uint16_t>(base) == 0x5A4D);
    const auto pe = base + memory.read<std::uint32_t>(base + 0x3C);
    REQUIRE(memory.read<std::uint32_t>(pe) == 0x4550);
    const auto directory = base + memory.read<std::uint32_t>(pe + 0x78);
    const auto names     = memory.read<std::uint32_t>(directory + 24);
    const auto functions = base + memory.read<std::uint32_t>(directory + 28);
    const auto nameTable = base + memory.read<std::uint32_t>(directory + 32);
    const auto ordinals  = base + memory.read<std::uint32_t>(directory + 36);
    for (auto i = 0u; i < names; ++i)
    {
        if (memory.readString(base + memory.read<std::uint32_t>(nameTable + 4 * i)) == name)
        {
            return base + memory.read<std::uint32_t>(functions + 4 * memory.read<std::uint16_t>(ordinals + 2 * i));
        }
    }
    return 0;
}

auto exportByOrdinal(Memory& memory, const std::uint32_t base, const std::uint32_t ordinal) -> std::uint32_t
{
    const auto pe        = base + memory.read<std::uint32_t>(base + 0x3C);
    const auto directory = base + memory.read<std::uint32_t>(pe + 0x78);
    const auto first     = memory.read<std::uint32_t>(directory + 16);
    const auto functions = base + memory.read<std::uint32_t>(directory + 28);
    return base + memory.read<std::uint32_t>(functions + 4 * (ordinal - first));
}

auto addSample(Machine& machine)
{
    auto& boundary = machine.boundary();
    boundary.add(Function{ "KERNEL32.dll", "GetTickCount", Convention::Stdcall, 0, [](Call&) -> Outcome
                           {
                               return Return{ 1234 };
                           } });
    boundary.add(Function{ "kernel32.dll", "Sleep", Convention::Stdcall, 1, [](Call&) -> Outcome
                           {
                               return Block{ Wait{ {}, false, 5 }, {} };
                           } });
    boundary.add(Function{ "kernel32.dll", "lstrlenA", Convention::Stdcall, 1, [](Call& call) -> Outcome
                           {
                               return Return{ static_cast<std::uint32_t>(call.argString(0).size()) };
                           } });
    boundary.add(Function{ "viewer", "table", Convention::Cdecl, 2, [](Call& call) -> Outcome
                           {
                               return Return{ call.arg(0) + call.arg(1), 7 };
                           } });
    boundary.add(Function{ "dsound.dll", "#11", Convention::Stdcall, 3, {} });
    boundary.publish();
}

// A guest stack with a return address and arguments; returns the stack pointer
auto pushCall(Memory& memory, Context& context, std::initializer_list<std::uint32_t> args) -> std::uint32_t
{
    memory.map(0x11F00000, 0x1000);
    auto sp = std::uint32_t{ 0x11F00800 };
    memory.write<std::uint32_t>(sp, 0x10001234);
    auto offset = 4u;
    for (const auto value : args)
    {
        memory.write<std::uint32_t>(sp + offset, value);
        offset += 4;
    }
    context.r[kEsp] = sp;
    return sp;
}

} // namespace

TEST_CASE("Synthetic modules are PE images whose exports reach the stand-in slots")
{
    auto machine = Machine{};
    addSample(machine);
    auto& memory   = machine.memory();
    auto& boundary = machine.boundary();

    const auto kernel32 = boundary.module("Kernel32.DLL");
    REQUIRE(kernel32);
    CHECK((*kernel32 & 0xFFFF) == 0);
    const auto tick = exportByName(memory, *kernel32, "GetTickCount");
    CHECK(tick == boundary.resolve("kernel32.dll", "GetTickCount"));
    CHECK(exportByName(memory, *kernel32, "lstrlenA") == boundary.resolve("KERNEL32.DLL", "lstrlenA"));
    CHECK(exportByName(memory, *kernel32, "Nothing") == 0);
    // Scanning down from an export in 64 KiB steps finds the module's own header
    CHECK((tick & 0xFFFF0000) == *kernel32);

    const auto dsound = boundary.module("dsound.dll");
    REQUIRE(dsound);
    CHECK(exportByOrdinal(memory, *dsound, 11) == boundary.resolve("dsound.dll", "#11"));

    // The prologue a protector copies from, then the trap
    CHECK(memory.read<std::uint16_t>(tick) == 0xFF8B);
    CHECK(boundary.traps().contains(tick + TrapRange::kTrapOffset));
    CHECK_FALSE(boundary.traps().contains(tick));
    REQUIRE(boundary.find(tick + 5));
    CHECK(boundary.find(tick + 5)->name == "GetTickCount");
    CHECK(boundary.find(tick + TrapRange::kTrapOffset + 1) == nullptr);
    CHECK(boundary.find(*kernel32) == nullptr);
}

TEST_CASE("Dispatch runs the handler, the machine completes stdcall and cdecl calls, missing handlers halt")
{
    auto machine = Machine{};
    addSample(machine);
    auto& memory   = machine.memory();
    auto& boundary = machine.boundary();
    auto  context  = Context{};
    auto  observed = std::vector<std::string>{};
    boundary.observe([&](const Call& call)
                     {
                         observed.emplace_back(call.name());
                     });

    const auto sp   = pushCall(memory, context, { 0x11F00F00 });
    auto       trap = boundary.resolve("kernel32.dll", "lstrlenA") + TrapRange::kTrapOffset;
    memory.writeString(0x11F00F00, "four");
    auto outcome = boundary.dispatch(machine, context, trap);
    REQUIRE(std::holds_alternative<Return>(outcome));
    CHECK(std::get<Return>(outcome).eax == 4);
    CHECK(context.r[kEsp] == sp); // dispatch leaves the stack alone
    machine.complete(context, trap, std::get<Return>(outcome));
    CHECK(context.r[kEax] == 4);
    CHECK(context.eip == 0x10001234);
    CHECK(context.r[kEsp] == sp + 8);

    pushCall(memory, context, { 2, 3 });
    trap    = boundary.resolve("viewer", "table") + TrapRange::kTrapOffset;
    outcome = boundary.dispatch(machine, context, trap);
    machine.complete(context, trap, std::get<Return>(outcome));
    CHECK(context.r[kEax] == 5);
    CHECK(context.r[kEdx] == 7);
    CHECK(context.r[kEsp] == sp + 4); // the caller pops

    pushCall(memory, context, { 10 });
    outcome = boundary.dispatch(machine, context, boundary.resolve("kernel32.dll", "Sleep") + TrapRange::kTrapOffset);
    REQUIRE(std::holds_alternative<Block>(outcome));
    CHECK(context.r[kEsp] == sp);

    pushCall(memory, context, { 1, 2, 3 });
    outcome = boundary.dispatch(machine, context, boundary.resolve("dsound.dll", "#11") + TrapRange::kTrapOffset);
    REQUIRE(std::holds_alternative<Halt>(outcome));
    CHECK(std::get<Halt>(outcome).reason == "unhandled import dsound.dll!#11 from 0x10001234");
    CHECK(observed == std::vector<std::string>{ "kernel32.dll!lstrlenA", "viewer!table", "kernel32.dll!Sleep", "dsound.dll!#11" });

    // The callGuest return trap is a trap address, so returning there stops at once
    CHECK(boundary.traps().contains(boundary.returnTrap()));
    CHECK(boundary.find(boundary.returnTrap()));

    // Names nobody registered still resolve, to a slot that halts
    const auto late = boundary.resolve("user32.dll", "MessageBoxW");
    CHECK(late == boundary.resolve("USER32.dll", "MessageBoxW"));
    REQUIRE(boundary.find(late));
    pushCall(memory, context, {});
    outcome = boundary.dispatch(machine, context, late + TrapRange::kTrapOffset);
    CHECK(std::get<Halt>(outcome).reason.starts_with("unhandled import user32.dll!MessageBoxW"));
}

TEST_CASE("The main thread gets a TEB and the PEB the protector reads")
{
    auto  machine = Machine{};
    auto& memory  = machine.memory();
    auto& thread  = machine.scheduler().createMain(0x100000);
    CHECK(&machine.scheduler().current() == &thread);
    const auto teb = thread.context.fsBase;
    CHECK(teb == thread.teb);
    CHECK(memory.read<std::uint32_t>(teb) == 0xFFFFFFFF);
    CHECK(memory.read<std::uint32_t>(teb + 0x04) == thread.stackBase);
    CHECK(memory.read<std::uint32_t>(teb + 0x08) == thread.stackLimit);
    CHECK(memory.read<std::uint32_t>(teb + 0x18) == teb);
    CHECK(memory.read<std::uint32_t>(teb + 0x24) == thread.id);
    CHECK(thread.stackBase - thread.stackLimit == 0x100000);
    CHECK(memory.mapped(thread.stackLimit, 0x100000));
    const auto peb = memory.read<std::uint32_t>(teb + 0x30);
    CHECK(memory.read<std::uint8_t>(peb + 2) == 0);
    CHECK(memory.read<std::uint32_t>(peb + 0x68) == 0);
    CHECK(memory.read<std::uint32_t>(peb + 0x08) == layout::kGameImage);
    auto& other = machine.scheduler().create(0x10001000, 0, 0x10000, true, 0);
    CHECK(memory.read<std::uint32_t>(other.teb + 0x30) == peb);
}

TEST_CASE("Structured exceptions walk the fs:[0] chain and resume from the edited CONTEXT")
{
    auto machine = Machine{};
    machine.boundary().publish();
    auto& memory  = machine.memory();
    auto& thread  = machine.scheduler().createMain(0x10000);
    auto& context = thread.context;

    // Two registration records on the stack: the inner one declines, the outer one handles
    const auto outer = thread.stackBase - 0x40;
    const auto inner = thread.stackBase - 0x80;
    memory.write<std::uint32_t>(outer, 0xFFFFFFFF);
    memory.write<std::uint32_t>(outer + 4, 0x10002000);
    memory.write<std::uint32_t>(inner, outer);
    memory.write<std::uint32_t>(inner + 4, 0x10001000);
    memory.write<std::uint32_t>(context.fsBase, inner);
    context.r[kEsp] = thread.stackBase - 0x100;
    context.r[kEbx] = 0x55;
    const auto esp  = context.r[kEsp];

    const auto information = std::array<std::uint32_t, 2>{ 1, 0x1234 };
    REQUIRE(machine.raise(kAccessViolation, 0x10000500, information));
    CHECK(context.eip == 0x10001000);
    // handler(record, frame, context, dispatcher) with the return trap as return address
    const auto record        = memory.read<std::uint32_t>(context.r[kEsp] + 4);
    const auto contextRecord = memory.read<std::uint32_t>(context.r[kEsp] + 12);
    CHECK(memory.read<std::uint32_t>(context.r[kEsp] + 8) == inner);
    CHECK(memory.read<std::uint32_t>(record) == kAccessViolation);
    CHECK(memory.read<std::uint32_t>(record + 12) == 0x10000500);
    CHECK(memory.read<std::uint32_t>(record + 16) == 2);
    CHECK(memory.read<std::uint32_t>(record + 24) == 0x1234);
    CHECK(memory.read<std::uint32_t>(contextRecord + 0xB8) == 0x10000500);
    CHECK(memory.read<std::uint32_t>(contextRecord + 0xA4) == 0x55);
    CHECK(memory.read<std::uint32_t>(contextRecord + 0xC4) == esp);

    // The handler returns ExceptionContinueSearch: pop the return address into the trap
    const auto returnFromHandler = [&](const std::uint32_t disposition)
    {
        context.r[kEax] = disposition;
        context.eip     = memory.read<std::uint32_t>(context.r[kEsp]);
        context.r[kEsp] += 4;
        REQUIRE(machine.boundary().traps().contains(context.eip));
        return machine.boundary().dispatch(machine, context, context.eip);
    };
    CHECK(std::holds_alternative<Resume>(returnFromHandler(1)));
    CHECK(context.eip == 0x10002000);
    CHECK(memory.read<std::uint32_t>(context.r[kEsp] + 8) == outer);

    // The outer one skips the faulting instruction and continues
    memory.write<std::uint32_t>(contextRecord + 0xB8, 0x10000502);
    memory.write<std::uint32_t>(contextRecord + 0xB0, 0x77);
    CHECK(std::holds_alternative<Resume>(returnFromHandler(0)));
    CHECK(context.eip == 0x10000502);
    CHECK(context.r[kEax] == 0x77);
    CHECK(context.r[kEbx] == 0x55);
    CHECK(context.r[kEsp] == esp);

    // With nothing left in the chain the exception is unhandled
    REQUIRE(machine.raise(kIntegerDivide, 0x10000600, {}));
    returnFromHandler(1);
    const auto last = returnFromHandler(1);
    REQUIRE(std::holds_alternative<Halt>(last));
    CHECK(std::get<Halt>(last).reason == "unhandled exception 0xc0000094 at 0x10000600");

    memory.write<std::uint32_t>(context.fsBase, 0xFFFFFFFF);
    CHECK_FALSE(machine.raise(kBreakpoint, 0x10000700, {}));
}

TEST_CASE("Slots made before publish keep their place beside the module images")
{
    auto       machine  = Machine{};
    auto&      memory   = machine.memory();
    auto&      boundary = machine.boundary();
    const auto methods  = std::array{ Method{ "QueryInterface", 3, {} }, Method{ "AddRef", 1, {} }, Method{ "Release", 1, {} } };
    const auto vtable   = boundary.addInterface("IEarly", methods);
    addSample(machine);
    const auto release = memory.read<std::uint32_t>(vtable + 8);
    REQUIRE(boundary.find(release));
    CHECK(boundary.find(release)->name == "Release");
    CHECK(release != boundary.resolve("kernel32.dll", "GetTickCount"));
    CHECK(findExport(memory, *boundary.module("kernel32.dll"), "GetTickCount") == boundary.resolve("kernel32.dll", "GetTickCount"));
    CHECK_FALSE(boundary.module("iearly"));
}
