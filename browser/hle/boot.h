#pragma once

// The milestone-2 boot: TM.dll through its protector, then the COM start-up tmloader does (src/app/loader.cpp):
// DllMain, DllGetClassObject, IClassFactory::CreateInstance and GameStart. By default the Viewer is a stand-in: a
// core object whose vtable and common-function table record every call and answer as the spike's did (ticket polls
// report done, everything else returns 0). With BootOptions::realViewer the user's polcore_native.dll runs instead
// (hle/viewer.h), set up between CreateInstance and GameStart as tmloader sets it up.
//
// The steps run as guest calls on the main thread, chained through a stand-in: each returns into the boot.next trap,
// which checks the result and stages the next call. So the whole boot runs under Machine::run (or any other CPU
// loop), with nothing nested and nothing to drive from outside.

#include "hle/process.h"
#include "hle/viewer.h"

#include <functional>
#include <map>
#include <memory>

namespace tmweb::hle
{

constexpr auto kInstallRoot = "C:\\PlayOnline\\SquareEnix\\TetraMaster";

enum class Step : std::uint8_t
{
    DllMain,
    GetClassObject,
    CreateInstance,
    GameStart,
    Viewer, // the real Viewer's set-up, between CreateInstance and GameStart
    Probe,  // the real Viewer's scripted Game IRC login, instead of TM.dll
};

struct BootOptions
{
    bool        realViewer     = false; // polcore_native.dll instead of the stand-in Viewer
    bool        probe          = false; // with realViewer: skip TM.dll, log in to Game IRC and load the profile, then halt
    bool        keygenStandIns = true;  // host code for the RSA key generation's hot routines
    ViewerLogin login;
};

struct ViewerCalls
{
    std::map<unsigned, std::uint64_t> core;   // core object vtable slots
    std::map<unsigned, std::uint64_t> second; // the interface core slot 3 hands out
    std::map<unsigned, std::uint64_t> table;  // common-function table entries
};

class Boot final
{
public:
    // Register the stand-ins and the Viewer, publish the boundary, map TM_native.dll from the runtime folder, create
    // the main thread and stage DllMain on it. The stand-ins keep the boot alive as long as the machine. Throws
    // std::runtime_error when TM_native.dll is missing or not a PE file.
    static auto start(Machine& machine, std::shared_ptr<HostFiles> files, BootOptions options = {}) -> std::shared_ptr<Boot>;

    Boot(Machine& machine, std::shared_ptr<HostFiles> files, BootOptions options = {});

    // Called with each step that returned successfully, before the next one is staged
    std::function<void(Step)> onStep;

    auto process() -> Process&;
    auto viewer() const -> const ViewerCalls&;
    auto gameStart() const -> std::uint32_t;
    // The real Viewer, or nothing with the stand-in
    auto realViewer() -> Viewer*;

private:
    void install();
    void stage(std::uint32_t function, std::initializer_list<std::uint32_t> args);
    auto blob(std::span<const std::uint8_t> bytes) -> std::uint32_t;
    // The boot.next trap: a step returned with eax
    auto next(std::uint32_t eax) -> Outcome;
    auto startGame() -> Outcome;
    // Run the Viewer's chain on the main thread from the top of its stack; it returns into boot.next
    void startViewer(std::deque<GuestCall> calls);

    Machine&                   machine_;
    std::shared_ptr<HostFiles> files_;
    Process                    process_;
    BootOptions                options_;
    std::unique_ptr<Viewer>    realViewer_;
    ViewerCalls                viewer_;
    Step                       step_    = Step::DllMain;
    std::uint32_t              out_     = 0; // the out pointer of the staged call
    std::uint32_t              factory_ = 0;
    std::uint32_t              game_    = 0;
    std::uint32_t              start_   = 0;
    std::uint32_t              core_    = 0;
    std::uint32_t              second_  = 0;
    std::uint32_t              table_   = 0;
    std::uint32_t              strings_ = 0;
};

} // namespace tmweb::hle
