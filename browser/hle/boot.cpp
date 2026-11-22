#include "hle/boot.h"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>

namespace tmweb::hle
{
namespace
{

using Guid = std::array<std::uint8_t, 16>;

// GUIDs in memory order (Data1..3 little-endian), from tmloader's src/app/loader.cpp and abi.h
constexpr auto kGameClass     = Guid{ 0x03, 0xfe, 0xb2, 0x72, 0x77, 0xba, 0x67, 0x48, 0x84, 0xa2, 0x7b, 0xad, 0x0f, 0x5a, 0x8f, 0xbb };
constexpr auto kGameInterface = Guid{ 0x29, 0x70, 0x28, 0xfa, 0xc4, 0x5e, 0x38, 0x49, 0xb8, 0xc5, 0x53, 0x28, 0x65, 0xdd, 0xe3, 0x69 };
constexpr auto kClassFactory  = Guid{ 0x01, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
constexpr auto kCoreInterface = Guid{ 0x54, 0x66, 0x51, 0xe0, 0x77, 0xef, 0x5d, 0x43, 0xaa, 0x7d, 0x50, 0xd2, 0xc0, 0x69, 0xce, 0x34 };

constexpr auto     kViewer         = "viewer.stub";
constexpr unsigned kVtableSlots    = 64;
constexpr unsigned kTableEntries   = 1200; // ViewerCommandCount
constexpr unsigned kGameStartSlot  = 9;
constexpr unsigned kCreateInstance = 3;
constexpr unsigned kLaunchWindowed = 0x200;
constexpr auto     kRegistryRoot   = "SOFTWARE\\PlayOnlineUS\\SquareEnix";
constexpr auto     kStackHeadroom  = 0x100u;
constexpr auto     kMainStack      = 0x100000u;

// Stack arguments after this, for the core slots tmloader knows; other slots halt as unhandled
const auto kCoreArgs   = std::map<unsigned, std::uint8_t>{ { 0, 2 }, { 1, 0 }, { 2, 0 }, { 3, 1 }, { 4, 1 }, { 5, 2 }, { 7, 1 }, { 26, 1 }, { 27, 1 } };
const auto kSecondArgs = std::map<unsigned, std::uint8_t>{ { 0, 2 }, { 1, 0 }, { 2, 0 } };
// Ticket polls the boot waits on report done. Past the boot, the frame loop's entries answer as polcore does when
// idle, measured on polcore itself under tools/polcore (run_polcore.py's set-up, then the entry): 832 and 844
// release what 831 and 843 handed out, 155 and 911 report ready, 190 has no pending message (-1), 696 passes
// GetTickCount through, and 705, the periodic tick, returns 0. There is no server: the Game IRC open (132) gives
// slot 0, and its check (133) fails with -33 on every poll while the pump (153) returns 1, as polcore does when
// the connection is refused. TM then shows its "error while connecting to the server" screen.
const auto         kTableResults = std::map<unsigned, std::uint32_t>{ { 132, 0 }, { 133, 0xFFFFFFDF }, { 153, 1 }, { 155, 1 }, { 190, 0xFFFFFFFF }, { 471, 1 }, { 473, 1 }, { 832, 1 }, { 844, 1 }, { 908, 1 }, { 911, 1 } };
constexpr unsigned kTickCount    = 696;
// Strings the frame loop copies and hashes: 831 the signed-in POL ID, 843 the message hash key. The stand-in has
// no sign-in, so it answers with tools/polcore's test account and a placeholder key, not polcore's.
const auto     kTableStrings = std::map<unsigned, std::pair<std::uint32_t, const char*>>{ { 831, { 48, "EEEEEEEF" } }, { 843, { 64, "stand-in-key" } } };
constexpr auto kStringsSize  = 96u;

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

auto slotName(const bool second, const unsigned slot) -> std::string
{
    return std::format("{}.{}", second ? "second" : "core", slot);
}

} // namespace

auto Boot::start(Machine& machine, std::shared_ptr<HostFiles> files, BootOptions options) -> std::shared_ptr<Boot>
{
    auto boot = std::make_shared<Boot>(machine, std::move(files), std::move(options));
    machine.boundary().add(Function{ "tmweb", "boot.next", Convention::Cdecl, 0, [boot](Call& call)
                                     {
                                         return boot->next(call.context().r[kEax]);
                                     } });
    boot->install();
    return boot;
}

Boot::Boot(Machine& machine, std::shared_ptr<HostFiles> files, BootOptions options)
: machine_(machine)
, files_(std::move(files))
, process_(machine, *files_, kInstallRoot)
, options_(std::move(options))
{
    if (options_.realViewer)
    {
        realViewer_ = std::make_unique<Viewer>(process_, options_.login, options_.keygenStandIns);
    }
}

void Boot::install()
{
    auto& boundary = machine_.boundary();
    auto& memory   = machine_.memory();
    auto& p        = process_;
    hle::install(p);

    // Core slots. QueryInterface hands back the object itself; slot 3 gives the second interface, 4 the command
    // line, 7 the common-function table, 26 the registry root.
    const auto coreSlot = [&](const bool second, const unsigned slot, const std::uint8_t argc)
    {
        boundary.add(Function{ kViewer, slotName(second, slot), Convention::Stdcall, static_cast<std::uint8_t>(argc + 1), [this, &memory, second, slot](Call& call)
                               {
                                   ++(second ? viewer_.second : viewer_.core)[slot];
                                   process_.trace.push_back(std::format("{} {}", second ? "second" : "core", slot));
                                   const auto out = call.arg(slot == 0 ? 2 : 1);
                                   if (slot == 0)
                                   {
                                       memory.write<std::uint32_t>(out, second ? second_ : core_);
                                       return ret(0);
                                   }
                                   if (slot == 1 || slot == 2)
                                   {
                                       return ret(1); // AddRef, Release
                                   }
                                   if (second)
                                   {
                                       return ret(0);
                                   }
                                   switch (slot)
                                   {
                                       case 3:
                                           memory.write<std::uint32_t>(out, second_);
                                           break;
                                       case 4:
                                           memory.write<std::uint32_t>(out, strings_); // an empty command line
                                           break;
                                       case 7:
                                           memory.write<std::uint32_t>(out, table_);
                                           break;
                                       case 26:
                                           memory.write<std::uint32_t>(out, strings_ + 4);
                                           break;
                                       default:
                                           break;
                                   }
                                   return ret(0);
                               } });
    };
    for (const auto [slot, argc] : kCoreArgs)
    {
        coreSlot(false, slot, argc);
    }
    for (const auto [slot, argc] : kSecondArgs)
    {
        coreSlot(true, slot, argc);
    }
    for (auto entry = 0u; entry < kTableEntries; ++entry)
    {
        boundary.add(Function{ kViewer, std::format("table.{}", entry), Convention::Cdecl, 0, [this, entry](Call& call)
                               {
                                   ++viewer_.table[entry];
                                   process_.trace.push_back(std::format("table {} {:#x} {:#x} {:#x} {:#x}", entry, call.arg(0), call.arg(1), call.arg(2), call.arg(3)));
                                   if (const auto text = kTableStrings.find(entry); text != kTableStrings.end())
                                   {
                                       return ret(strings_ + text->second.first);
                                   }
                                   if (entry == kTickCount)
                                   {
                                       return ret(static_cast<std::uint32_t>(machine_.scheduler().now() + 1000));
                                   }
                                   const auto result = kTableResults.find(entry);
                                   return ret(result == kTableResults.end() ? 0 : result->second);
                               } });
    }

    if (realViewer_)
    {
        realViewer_->install();
    }
    boundary.publish();
    finishInstall(p);

    // The objects: { vtable } with 64 slots each, the table, and a strings block: the command line, the root
    const auto object = [&](const bool second)
    {
        const auto vtable = p.allocate(4 * kVtableSlots);
        for (auto slot = 0u; slot < kVtableSlots; ++slot)
        {
            memory.write<std::uint32_t>(vtable + 4 * slot, boundary.resolve(kViewer, slotName(second, slot)));
        }
        const auto address = p.allocate(4);
        memory.write<std::uint32_t>(address, vtable);
        return address;
    };
    core_   = object(false);
    second_ = object(true);
    table_  = p.allocate(4 * kTableEntries);
    for (auto entry = 0u; entry < kTableEntries; ++entry)
    {
        memory.write<std::uint32_t>(table_ + 4 * entry, boundary.resolve(kViewer, std::format("table.{}", entry)));
    }
    strings_ = p.allocate(kStringsSize);
    memory.writeString(strings_ + 4, kRegistryRoot);
    for (const auto& [entry, text] : kTableStrings)
    {
        memory.writeString(strings_ + text.first, text.second);
    }

    auto file = files_->read(p.gameFile);
    if (!file)
    {
        throw std::runtime_error(std::format("{} is not in the runtime folder", p.gameFile));
    }
    p.game = loadImage(memory, boundary, *file);
    if (realViewer_)
    {
        const auto viewer = files_->read(p.viewerFile);
        if (!viewer)
        {
            throw std::runtime_error(std::format("{} is not in the runtime folder", p.viewerFile));
        }
        realViewer_->load(*viewer);
    }
    machine_.scheduler().createMain(kMainStack);
    if (realViewer_ && options_.probe)
    {
        step_      = Step::Probe;
        auto calls = realViewer_->setup();
        for (auto& call : realViewer_->probe())
        {
            calls.push_back(std::move(call));
        }
        startViewer(std::move(calls));
        return;
    }
    stage(p.game.entry, { p.game.base, 1, 0 }); // DllMain(DLL_PROCESS_ATTACH)
}

void Boot::startViewer(std::deque<GuestCall> calls)
{
    auto& thread           = machine_.scheduler().current();
    thread.context.r[kEsp] = thread.stackBase - kStackHeadroom;
    realViewer_->start(std::move(calls), machine_.boundary().resolve("tmweb", "boot.next") + TrapRange::kTrapOffset, []
                       {
                           return 0u;
                       });
}

auto Boot::blob(const std::span<const std::uint8_t> bytes) -> std::uint32_t
{
    const auto address = process_.allocate(static_cast<std::uint32_t>(bytes.size()));
    machine_.memory().write(address, bytes);
    return address;
}

// Each step starts from the top of the main stack and returns into boot.next
void Boot::stage(const std::uint32_t function, const std::initializer_list<std::uint32_t> args)
{
    auto& memory    = machine_.memory();
    auto& thread    = machine_.scheduler().current();
    auto  sp        = thread.stackBase - kStackHeadroom - 4 * static_cast<std::uint32_t>(args.size() + 1);
    auto& context   = thread.context;
    context.r[kEsp] = sp;
    context.eip     = function;
    memory.write<std::uint32_t>(sp, machine_.boundary().resolve("tmweb", "boot.next") + TrapRange::kTrapOffset);
    for (const auto value : args)
    {
        sp += 4;
        memory.write<std::uint32_t>(sp, value);
    }
}

auto Boot::next(const std::uint32_t eax) -> Outcome
{
    auto& memory = machine_.memory();
    switch (step_)
    {
        case Step::DllMain:
        {
            if (eax != 1)
            {
                return Halt{ std::format("DllMain returned {}", eax) };
            }
            const auto getObject = findExport(memory, process_.game.base, "DllGetClassObject");
            if (!getObject)
            {
                return Halt{ "TM_native.dll does not export DllGetClassObject" };
            }
            if (onStep)
            {
                onStep(step_);
            }
            step_ = Step::GetClassObject;
            out_  = process_.allocate(4);
            stage(getObject, { blob(kGameClass), blob(kClassFactory), out_ });
            return Resume{};
        }
        case Step::GetClassObject:
            factory_ = memory.read<std::uint32_t>(out_);
            if (eax != 0 || !factory_)
            {
                return Halt{ std::format("DllGetClassObject returned {:#x}", eax) };
            }
            if (onStep)
            {
                onStep(step_);
            }
            step_ = Step::CreateInstance;
            out_  = process_.allocate(4);
            stage(memory.read<std::uint32_t>(memory.read<std::uint32_t>(factory_) + 4 * kCreateInstance), { factory_, 0, blob(kGameInterface), out_ });
            return Resume{};
        case Step::CreateInstance:
        {
            game_ = memory.read<std::uint32_t>(out_);
            if (eax != 0 || !game_)
            {
                return Halt{ std::format("CreateInstance returned {:#x}", eax) };
            }
            start_ = memory.read<std::uint32_t>(memory.read<std::uint32_t>(game_) + 4 * kGameStartSlot);
            if (onStep)
            {
                onStep(step_);
            }
            if (realViewer_)
            {
                step_ = Step::Viewer;
                startViewer(realViewer_->setup());
                return Resume{};
            }
            return startGame();
        }
        case Step::Viewer:
            core_ = realViewer_->core();
            if (onStep)
            {
                onStep(step_);
            }
            return startGame();
        case Step::Probe:
        {
            const auto& results = realViewer_->profileResults();
            const auto  result  = [&](const unsigned request)
            {
                const auto found = results.find(request);
                return found == results.end() ? std::string{ "none" } : std::to_string(static_cast<std::int32_t>(found->second));
            };
            if (onStep)
            {
                onStep(step_);
            }
            return Halt{ std::format("viewer probe: Game IRC login accepted; profile handles {}, characters {}, status {}", result(171), result(175), result(193)) };
        }
        case Step::GameStart:
            if (onStep)
            {
                onStep(step_);
            }
            return Halt{ std::format("GameStart returned {:#x}", eax) };
    }
    return Halt{ "boot.next after the last step" };
}

auto Boot::startGame() -> Outcome
{
    step_ = Step::GameStart;
    // LaunchDescriptor { size, flags, core interface id, message }
    auto descriptor = std::array<std::uint8_t, 28>{};
    descriptor[0]   = 28;
    descriptor[5]   = kLaunchWindowed >> 8;
    std::ranges::copy(kCoreInterface, descriptor.begin() + 8);
    stage(start_, { game_, core_, blob(descriptor) });
    return Resume{};
}

auto Boot::realViewer() -> Viewer*
{
    return realViewer_.get();
}

auto Boot::process() -> Process&
{
    return process_;
}

auto Boot::viewer() const -> const ViewerCalls&
{
    return viewer_;
}

auto Boot::gameStart() const -> std::uint32_t
{
    return start_;
}

} // namespace tmweb::hle
