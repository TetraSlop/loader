// The user's TM_native.dll through its protector to Direct3DCreate8, compared with the spike's Python run on
// Unicorn (WASM_SPIKE.md, "Boot to the first Direct3D call"). Needs TMWEB_RUNTIME, the runtime folder; skipped
// without it. With TMWEB_TM_MAPPED (tmserver's verified TM_emulated_mapped.dll) the unpacked image is compared too.
#include "hle/boot.h"
#include "tests/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <set>

using namespace tmweb;
using namespace tmweb::hle;

namespace
{

// The real import table the protector rebuilds
constexpr std::uint32_t kIatStart = 0x204000;
constexpr std::uint32_t kIatEnd   = 0x204278;
// Code, read-only data and data: what tmserver's mapped image verifies
constexpr std::uint32_t kImageStart = 0x8000;
constexpr std::uint32_t kImageEnd   = 0x32A000;

auto environment(const char* name) -> std::string
{
    const auto* value = std::getenv(name);
    return value ? value : "";
}

} // namespace

TEST_CASE("TM_native.dll unpacks and boots to Direct3DCreate8(220)" TMWEB_ENGINE)
{
    const auto runtime = environment("TMWEB_RUNTIME");
    if (runtime.empty())
    {
        SKIP("TMWEB_RUNTIME is not set");
    }

    auto  machine = Machine{};
    auto& memory  = machine.memory();
    auto  calls   = std::map<std::string, int>{};
    auto  phase   = Step::DllMain;
    auto  last    = std::string{};
    // The unpacked image when the protector hands over to the game's CRT, whose first call is GetVersion
    auto image = std::vector<std::uint8_t>{};
    machine.boundary().observe(
        [&](const Call& call)
        {
            auto& scheduler = call.machine().scheduler();
            // A thread's exit trap runs with esp at its stack base, so the return address may be unreadable
            const auto esp = call.context().r[kEsp];
            last           = std::format("{} from {:#x} on thread {}", call.name(), call.memory().mapped(esp, 4) ? call.returnAddress() : 0, scheduler.current().id);
            // The main thread's calls, as the spike counted them; its fake threads never ran
            if (&scheduler.current() == scheduler.threads().front().get())
            {
                calls[std::format("{} {}", static_cast<int>(phase), call.name())]++;
            }
            if (image.empty() && call.name() == "kernel32.dll!GetVersion")
            {
                image.resize(kImageEnd - kImageStart);
                memory.read(layout::kGameImage + kImageStart, image);
            }
        });
    const auto boot  = Boot::start(machine, std::make_shared<DiskFiles>(runtime));
    auto&      p     = boot->process();
    auto       trace = std::size_t{};
    boot->onStep     = [&](const Step step)
    {
        if (step == Step::CreateInstance)
        {
            trace = p.trace.size();
        }
        phase = static_cast<Step>(static_cast<int>(step) + 1);
    };

    auto       engine = test::Engine{ machine };
    const auto result = engine.run(test::Engine::kBudget);
    INFO(std::format("{}; the last stand-in call was {}", result.reason, last));
    REQUIRE(result.reason.starts_with("unhandled import d3d8.dll!Direct3DCreate8 from "));
    CHECK(memory.read<std::uint32_t>(machine.scheduler().current().context.r[kEsp] + 4) == 220);
    CHECK(boot->gameStart() == p.game.base + 0x8210);
    // The protector's deliberate faults, all inside DllMain
    CHECK(result.exceptions == 20);

    // Byte for byte, except the IAT: its entries are this run's stand-in and thunk addresses
    const auto mapped = environment("TMWEB_TM_MAPPED");
    if (!mapped.empty())
    {
        auto file      = std::ifstream(mapped, std::ios::binary);
        auto reference = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
        REQUIRE(reference.size() >= kImageEnd);
        REQUIRE(image.size() == kImageEnd - kImageStart);
        auto differ = std::vector<std::uint32_t>{};
        for (auto i = 0u; i < image.size(); ++i)
        {
            const auto rva = kImageStart + i;
            if (image[i] != reference[rva] && (rva < kIatStart || rva >= kIatEnd))
            {
                differ.push_back(rva);
            }
        }
        INFO(std::format("{} bytes differ from {:#x}", differ.size(), differ.empty() ? 0 : differ.front()));
        CHECK(differ.empty());
    }

    // Name every IAT slot: a stand-in directly, or a protector thunk run until it reaches one
    auto       names   = std::map<std::uint32_t, std::string>{};
    auto       modules = std::map<std::string, int>{};
    const auto saved   = machine.scheduler().current().context;
    for (auto rva = kIatStart; rva < kIatEnd; rva += 4)
    {
        const auto target = memory.read<std::uint32_t>(p.game.base + rva);
        if (target == 0)
        {
            continue;
        }
        const auto* function = machine.boundary().find(target);
        if (!function)
        {
            auto probe      = saved;
            probe.eip       = target;
            probe.r[kEsp]   = machine.scheduler().current().stackBase - 0x8000;
            const auto stop = engine.probe(probe, 10000);
            if (stop.stop == Stop::Trap)
            {
                function = machine.boundary().find(stop.address);
            }
        }
        INFO(std::format("slot {:#x} holds {:#x}", rva, target));
        REQUIRE(function);
        names[rva] = function->module + "!" + function->name;
        ++modules[function->module];
    }
    CHECK(names.size() == 148);
    CHECK(modules == std::map<std::string, int>{ { "kernel32.dll", 92 }, { "user32.dll", 33 }, { "imm32.dll", 6 }, { "oleaut32.dll", 6 }, { "advapi32.dll", 4 }, { "ole32.dll", 3 }, { "d3d8.dll", 1 }, { "dinput8.dll", 1 }, { "dsound.dll", 1 }, { "gdi32.dll", 1 } });
    // The slots tmloader patches by address (src/platform/system.cpp)
    CHECK(names[0x204000] == "advapi32.dll!RegOpenKeyA");
    CHECK(names[0x204004] == "advapi32.dll!RegQueryValueExA");
    CHECK(names[0x204008] == "advapi32.dll!RegCloseKey");
    CHECK(names[0x20400C] == "advapi32.dll!RegCreateKeyExA");
    CHECK(names[0x2041FC] == "user32.dll!MessageBoxA");

    // What GameStart called before Direct3DCreate8, as the spike counted it
    auto imports = std::set<std::string>{};
    for (const auto& [key, count] : calls)
    {
        if (key.starts_with("3 ") && !key.contains("viewer.stub!") && !key.contains("tmweb!") && !key.contains("ntdll.dll!"))
        {
            imports.insert(key.substr(key.find('!') + 1));
        }
    }
    CHECK(imports == std::set<std::string>{ "#11", "AttachThreadInput", "CloseHandle", "CoInitialize", "CreateFileA", "CreateMutexA", "CreateThread", "CreateWindowExA", "Direct3DCreate8", "EnterCriticalSection", "FindFirstFileA", "FindNextFileA", "GetFileType", "GetForegroundWindow", "GetLastError", "GetStockObject", "GetVersionExA", "GetWindowThreadProcessId", "HeapAlloc", "HeapFree", "ImmDisableIME", "InitializeCriticalSection", "LeaveCriticalSection", "LoadCursorA", "LoadIconA", "ReadFile", "RegCloseKey", "RegCreateKeyExA", "RegQueryValueExA", "RegisterClassExA", "ReleaseMutex", "SetFilePointer", "SetForegroundWindow", "SetLastError", "SetThreadExecutionState", "SetThreadPriority", "SetWindowsHookExA", "ShowCursor", "SystemParametersInfoA", "TlsGetValue", "WaitForSingleObject" });

    const auto& viewer = boot->viewer();
    auto        core   = std::set<unsigned>{};
    for (const auto& [slot, count] : viewer.core)
    {
        core.insert(slot);
    }
    CHECK(core == std::set<unsigned>{ 0, 1, 3, 4, 7, 26 });
    auto table = std::set<unsigned>{};
    for (const auto& [entry, count] : viewer.table)
    {
        table.insert(entry);
    }
    CHECK(table == std::set<unsigned>{ 126, 288, 362, 470, 471, 472, 473, 824, 907, 908, 966 });

    auto opens = 0;
    for (auto i = trace; i < p.trace.size(); ++i)
    {
        opens += p.trace[i].starts_with("CreateFileA ");
    }
    CHECK(opens == 59);
    REQUIRE(p.classes.size() == 1);
    CHECK(p.classes[0].name == "Tetra Master");
    CHECK(p.classes[0].procedure == p.game.base + 0x1863D0);
    REQUIRE(p.windows.size() == 1);
    CHECK(p.windows[0].width == 640);
    CHECK(p.windows[0].height == 480);
    REQUIRE(p.hooks.size() == 1);
    CHECK(p.hooks[0].type == 13); // WH_KEYBOARD_LL
    CHECK(p.hooks[0].procedure == p.game.base + 0x8030);
    CHECK(std::ranges::count_if(p.trace, [&](const std::string& line)
                                {
                                    return line.starts_with(std::format("CreateThread {:#x} ", p.game.base + 0x1A71D0));
                                }) == 1);
    CHECK(std::ranges::find(p.trace, "DirectSoundCreate8") != p.trace.end());
}
