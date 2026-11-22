#pragma once

// The real Viewer: the user's polcore_native.dll, mapped at layout::kViewerImage and unpacked by its own POL1 stub,
// then set up as tmloader does without pol.exe (src/app/loader.cpp and src/platform/network.cpp): the core object,
// the area code, parameters, languages, registry root and install folder, the account, and the INET and profile
// clients. Two common-table entries are hooked as tmloader hooks them: Game IRC open (132) gets the server and the
// account, and the first open check (133) that succeeds seeds the profile cipher and loads the handles, characters
// and status the Viewer normally loads before it starts the game. browser/docs/polcore.md has the measurements.
//
// The internal addresses are valid only for the pinned build (SHA-256 73B1864B...); load() checks its PE identity
// and the setup checks the table's address, as tmloader does.
//
// Guest calls run as a chain on one thread: each returns into the viewer.chain trap, which takes its result and
// stages the next, so polls can Sleep and nothing nests inside a stand-in.

#include "hle/process.h"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tmweb::hle
{

struct ViewerLogin
{
    std::string   polId       = "EEEEEEEF"; // tmserver's first local account
    std::string   password    = "tm-local-test";
    std::string   server      = "127.0.0.1"; // the host routes streams by port; any IPv4 address serves
    std::uint16_t profilePort = 51220;
};

struct GuestCall;

// What a guest call's result leads to: an error halts, then runs before the rest of the chain
struct Next
{
    std::optional<std::string> error;
    std::vector<GuestCall>     then;
};

struct GuestCall
{
    std::string                            name; // for the log and errors
    std::uint32_t                          function = 0;
    std::vector<std::uint32_t>             args;
    Convention                             convention = Convention::Cdecl;
    std::function<Next(std::uint32_t eax)> done; // empty: go on
    std::function<GuestCall()>             make; // set: build the call when it is staged, from earlier results
};

class Viewer final
{
public:
    Viewer(Process& process, ViewerLogin login, bool keygenStandIns);

    // Before Boundary::publish: the chain trap, the hooks and the key-generation stand-ins
    void install();
    // After publish: map polcore at layout::kViewerImage. Throws std::runtime_error for another build.
    void load(std::span<const std::uint8_t> file);

    // Run calls on the current thread from its current stack, then continue at back with EAX set to result(). The
    // caller's handler returns Resume.
    void start(std::deque<GuestCall> calls, std::uint32_t back, std::function<std::uint32_t()> result);

    // DllMain, the core object and tmloader's set-up, in order
    auto setup() -> std::deque<GuestCall>;
    // Scripted login without TM.dll: Game IRC open through the hook, then the open check and poll every 10 ms until
    // it reports the login, which also loads the profile. Halts with the outcome.
    auto probe() -> std::deque<GuestCall>;

    auto core() const -> std::uint32_t;
    auto profileLoaded() const -> bool;
    // Each profile request's final check result, by request entry (171, 175, 193)
    auto profileResults() const -> const std::map<unsigned, std::uint32_t>&;

private:
    auto chainStep(Call& call) -> Outcome;
    void stageNext(Context& context);
    auto ircOpen(Call& call) -> Outcome;
    auto ircCheck(Call& call) -> Outcome;
    auto profileBootstrap(std::uint32_t slot) -> std::vector<GuestCall>;
    auto poll(unsigned request, std::uint32_t ticket, std::uint64_t started) -> GuestCall;
    void patchKeygen();
    auto rva(std::uint32_t offset) const -> std::uint32_t;
    auto string(std::string_view text) -> std::uint32_t;

    struct Chain
    {
        std::deque<GuestCall>          calls;
        std::uint32_t                  back = 0;
        std::function<std::uint32_t()> result;
        GuestCall                      running;
    };

    Process&                               process_;
    ViewerLogin                            login_;
    bool                                   keygen_;
    std::map<ThreadId, std::vector<Chain>> chains_;   // innermost last: a hooked call can run inside a chain
    std::vector<std::uint32_t>             original_; // the common table as set up, before the hooks
    std::uint32_t                          core_   = 0;
    std::uint32_t                          table_  = 0;
    std::uint32_t                          out_    = 0; // out pointers for the set-up calls
    bool                                   loaded_ = false;
    std::map<unsigned, std::uint32_t>      results_;
};

} // namespace tmweb::hle
