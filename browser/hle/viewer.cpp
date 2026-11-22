#include "hle/viewer.h"

#include "host/host.h"

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <stdexcept>

namespace tmweb::hle
{
namespace
{

using Guid = std::array<std::uint8_t, 16>;

// GUIDs in memory order, from tmloader's src/app/loader.cpp and abi.h
constexpr auto kCoreClass     = Guid{ 0xdd, 0xf5, 0x01, 0x35, 0x94, 0x78, 0xdf, 0x42, 0x86, 0x6a, 0xa2, 0xb6, 0x52, 0x7d, 0x80, 0x49 };
constexpr auto kClassFactory  = Guid{ 0x01, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
constexpr auto kCoreInterface = Guid{ 0x54, 0x66, 0x51, 0xe0, 0x77, 0xef, 0x5d, 0x43, 0xaa, 0x7d, 0x50, 0xd2, 0xc0, 0x69, 0xce, 0x34 };

// The pinned build's PE header (tmloader's src/config/validation.cpp pins its SHA-256)
constexpr std::uint32_t kTimeDateStamp = 0x4E4E5917;
constexpr std::uint32_t kSizeOfImage   = 0x451000;

// Game IRC open copies the password into a 16-byte buffer: a longer one overwrites polcore's stack
constexpr std::size_t kMaxPassword = 15;

constexpr unsigned kCreateInstance = 3; // IClassFactory slot
constexpr unsigned kSetParamInit   = 5; // core slots (app/abi.h)
constexpr unsigned kGetCommonTable = 7;
constexpr unsigned kSetAreaCode    = 27;
constexpr unsigned kTableEntries   = 1200;

// Common-table entries (app/abi.h)
enum Command : unsigned
{
    kSetInstallDirectory = 125,
    kGameIrcOpen         = 132,
    kGameIrcOpenCheck    = 133,
    kGameIrcPoll         = 153,
    kLoadHandle          = 171,
    kLoadCharacter       = 175,
    kLoadStatus          = 193,
    kSetRegistryRoot     = 367,
    kSetUiLanguage       = 420,
    kSetAccount          = 851,
    kEncodeAccount       = 854,
    kFileLayerStatus     = 932,
    kSetRegistryLanguage = 965,
};

// polcore internals, as RVAs (src/platform/network.cpp)
enum Internal : std::uint32_t
{
    kCommonTable        = 0x6FBE8,
    kInitializeInet     = 0xFE20,
    kResolveAddress     = 0x127A0,
    kInitializeProfile  = 0x1EB00,
    kSetProfileLogin    = 0x1EA60,
    kSetProfileEndpoint = 0x1E930,
    kProfileFollowUp    = 0x19BA0,
    kSeedProfileCipher  = 0x1A170,
    kSeedFollowUp       = 0x1E980,
    kProfileInitialized = 0xAFBD8,
    kProfileEndpoint    = 0x404AB8,
    kGameIrcSlots       = 0x3E58A0,
    // The RSA key generation's hot routines (browser/docs/polcore.md, "Encryption")
    kMontgomery = 0x2C1D0,
    kAdd512     = 0x2CA20,
    kShiftRight = 0x2CFA0,
};

constexpr std::uint32_t kGameIrcSlotSize     = 0x3A00;
constexpr std::uint32_t kGameIrcCipherKeys   = 0x2B8;
constexpr std::uint32_t kGameIrcCipher       = 0x2C0;
constexpr std::uint32_t kCipherSize          = 0x68;
constexpr std::uint32_t kCipherMatrixPointer = 0x48;
constexpr std::uint32_t kCipherMatrixSize    = 0x1000;
constexpr std::uint32_t kEndpointSize        = 20;

constexpr std::uint64_t kProfileTimeoutMs = 5000;  // per request, as tmloader waits
constexpr std::uint64_t kProbeTimeoutMs   = 30000; // the probe's whole Game IRC login
constexpr std::uint32_t kPollMs           = 10;

constexpr auto kCommandLine  = " /game DdcMLbB";
constexpr auto kRegistryRoot = "SOFTWARE\\PlayOnlineUS";
constexpr auto kClientLabel  = "46.49"; // the version TM passes to Game IRC open

auto tmweb(Boundary& boundary, const char* name) -> std::uint32_t
{
    return boundary.resolve("tmweb", name);
}

// 512-bit numbers as polcore keeps them: sixteen dwords, least significant first. Every read and write goes to
// guest memory in the original routine's order, so aliased arguments behave as they do there.
class Number
{
public:
    Number(Memory& memory, const std::uint32_t address)
    : memory_(memory)
    , address_(address)
    {
    }

    auto get(const unsigned dword) const -> std::uint32_t
    {
        return memory_.read<std::uint32_t>(address_ + 4 * dword);
    }

    void set(const unsigned dword, const std::uint32_t value) const
    {
        memory_.write<std::uint32_t>(address_ + 4 * dword, value);
    }

private:
    Memory&       memory_;
    std::uint32_t address_;
};

// The carry or borrow rule both routines use: compare the old word with the new one, keep the flag when equal
auto carryOut(const std::uint32_t before, const std::uint32_t after, const std::uint32_t carry) -> std::uint32_t
{
    return before > after ? 1 : before < after ? 0
                                               : carry;
}

auto borrowOut(const std::uint32_t before, const std::uint32_t after, const std::uint32_t borrow) -> std::uint32_t
{
    return before < after ? 1 : before > after ? 0
                                               : borrow;
}

// +2CA20: a += b, returning the carry
auto add512(Memory& memory, const std::uint32_t a, const std::uint32_t b) -> std::uint32_t
{
    const auto x = Number{ memory, a }, y = Number{ memory, b };
    auto       carry = std::uint32_t{};
    for (auto limb = 0u; limb < 8; ++limb)
    {
        const auto low  = x.get(2 * limb);
        const auto sumL = low + y.get(2 * limb) + carry;
        x.set(2 * limb, sumL);
        carry           = carryOut(low, sumL, carry);
        const auto high = x.get(2 * limb + 1);
        x.set(2 * limb + 1, high + carry);
        const auto sumH = high + carry + y.get(2 * limb + 1); // b's high word is read after a's is written
        x.set(2 * limb + 1, sumH);
        carry = carryOut(high, sumH, carry);
    }
    return carry;
}

// +2CA90: a -= b, returning the borrow
auto sub512(Memory& memory, const std::uint32_t a, const std::uint32_t b) -> std::uint32_t
{
    const auto x = Number{ memory, a }, y = Number{ memory, b };
    auto       borrow = std::uint32_t{};
    for (auto limb = 0u; limb < 8; ++limb)
    {
        const auto low   = x.get(2 * limb);
        const auto diffL = low - y.get(2 * limb) - borrow;
        x.set(2 * limb, diffL);
        borrow           = borrowOut(low, diffL, borrow);
        const auto high  = x.get(2 * limb + 1);
        const auto diffH = high - y.get(2 * limb + 1) - borrow;
        x.set(2 * limb + 1, diffH);
        borrow = borrowOut(high, diffH, borrow);
    }
    return borrow;
}

// +2CFA0: a >>= 1
void shiftRight512(Memory& memory, const std::uint32_t a)
{
    const auto x    = Number{ memory, a };
    auto       next = std::uint32_t{}; // the next limb's original low dword
    for (auto limb = 8u; limb-- > 0;)
    {
        const auto low = x.get(2 * limb), high = x.get(2 * limb + 1);
        x.set(2 * limb, (low >> 1) | (high << 31));
        x.set(2 * limb + 1, (high >> 1) | (next << 31));
        next = low;
    }
}

// +2D140: the index of the highest set bit, or -1
auto highestBit(Memory& memory, const std::uint32_t a) -> std::int32_t
{
    const auto x = Number{ memory, a };
    for (auto dword = 16u; dword-- > 0;)
    {
        if (const auto value = x.get(dword))
        {
            return static_cast<std::int32_t>(32 * dword + 31 - std::countl_zero(value));
        }
    }
    return -1;
}

// +2D2E0: 1 when a > b, 0 when equal, -1 when less (unsigned)
auto compare512(Memory& memory, const std::uint32_t a, const std::uint32_t b) -> std::uint32_t
{
    const auto x = Number{ memory, a }, y = Number{ memory, b };
    for (auto dword = 16u; dword-- > 0;)
    {
        if (x.get(dword) != y.get(dword))
        {
            return x.get(dword) > y.get(dword) ? 1 : 0xFFFFFFFF;
        }
    }
    return 0;
}

// +2C1D0(a, b, m, r): bitwise Montgomery multiplication, r = a * b / 2^(bits(m) + 1) mod m
auto montgomery(Memory& memory, const std::uint32_t a, const std::uint32_t b, const std::uint32_t m, const std::uint32_t r) -> std::uint32_t
{
    const auto bits = highestBit(memory, m) + 1;
    memory.fill(r, 0, 64);
    const auto x = Number{ memory, a }, y = Number{ memory, b }, result = Number{ memory, r };
    for (auto i = 0; i < bits; ++i)
    {
        const auto bit = (x.get(static_cast<unsigned>(i) >> 5) >> (i & 31)) & 1;
        const auto odd = ((bit & y.get(0)) ^ result.get(0)) & 1;
        if (bit)
        {
            add512(memory, r, b);
        }
        if (odd)
        {
            add512(memory, r, m);
        }
        shiftRight512(memory, r);
    }
    const auto order = compare512(memory, r, m);
    return order == 1 ? sub512(memory, r, m) : order;
}

} // namespace

Viewer::Viewer(Process& process, ViewerLogin login, const bool keygenStandIns)
: process_(process)
, login_(std::move(login))
, keygen_(keygenStandIns)
{
}

void Viewer::install()
{
    auto&      boundary = process_.machine.boundary();
    const auto add      = [&](const char* name, Handler handler)
    {
        boundary.add(Function{ "tmweb", name, Convention::Cdecl, 0, std::move(handler) });
    };
    add("viewer.chain", [this](Call& call)
        {
            return chainStep(call);
        });
    add("viewer.irc_open", [this](Call& call)
        {
            return ircOpen(call);
        });
    add("viewer.irc_check", [this](Call& call)
        {
            return ircCheck(call);
        });
    // The probe's Game IRC callbacks: TM's receive handlers; nothing calls them during the login
    add("viewer.callback", [this](Call& call) -> Outcome
        {
            process_.trace.push_back(std::format("viewer callback {:#x} {:#x}", call.arg(0), call.arg(1)));
            return Return{ 0 };
        });
    auto& memory = process_.machine.memory();
    add("viewer.montgomery", [&memory](Call& call) -> Outcome
        {
            return Return{ montgomery(memory, call.arg(0), call.arg(1), call.arg(2), call.arg(3)) };
        });
    add("viewer.add512", [&memory](Call& call) -> Outcome
        {
            return Return{ add512(memory, call.arg(0), call.arg(1)) };
        });
    add("viewer.shift_right512", [&memory](Call& call) -> Outcome
        {
            shiftRight512(memory, call.arg(0));
            return Return{ 0 }; // the routine's loop counter, which ends at 0
        });
}

void Viewer::load(const std::span<const std::uint8_t> file)
{
    const auto at = [&](const std::size_t offset)
    {
        if (offset + 4 > file.size())
        {
            throw std::runtime_error("polcore_native.dll is truncated");
        }
        auto value = std::uint32_t{};
        std::memcpy(&value, file.data() + offset, 4);
        return value;
    };
    const auto pe = at(0x3C);
    if (at(pe + 8) != kTimeDateStamp || at(pe + 24 + 56) != kSizeOfImage)
    {
        throw std::runtime_error("polcore_native.dll is not the supported build");
    }
    if (login_.password.size() > kMaxPassword)
    {
        throw std::runtime_error(std::format("the account's password has {} characters; polcore's Game IRC login takes at most {}", login_.password.size(), kMaxPassword));
    }
    auto& machine   = process_.machine;
    process_.viewer = loadImage(machine.memory(), machine.boundary(), file, layout::kViewerImage);
}

auto Viewer::rva(const std::uint32_t offset) const -> std::uint32_t
{
    return process_.viewer.base + offset;
}

auto Viewer::string(const std::string_view text) -> std::uint32_t
{
    const auto address = process_.allocate(static_cast<std::uint32_t>(text.size()) + 1);
    process_.machine.memory().writeString(address, text);
    return address;
}

void Viewer::start(std::deque<GuestCall> calls, const std::uint32_t back, std::function<std::uint32_t()> result)
{
    auto& thread = process_.thread();
    chains_[thread.id].push_back(Chain{ std::move(calls), back, std::move(result), {} });
    stageNext(thread.context);
}

// The next call of the thread's innermost chain, returning into viewer.chain; or, when it is done, back to its caller
void Viewer::stageNext(Context& context)
{
    auto& memory = process_.machine.memory();
    auto& stack  = chains_[process_.thread().id];
    auto& chain  = stack.back();
    if (chain.calls.empty())
    {
        context.eip     = chain.back;
        context.r[kEax] = chain.result ? chain.result() : 0;
        stack.pop_back();
        return;
    }
    chain.running = std::move(chain.calls.front());
    chain.calls.pop_front();
    while (chain.running.make)
    {
        chain.running = chain.running.make();
    }
    const auto& call = chain.running;
    auto        sp   = context.r[kEsp] - 4 * static_cast<std::uint32_t>(call.args.size() + 1);
    context.r[kEsp]  = sp;
    context.eip      = call.function;
    memory.write<std::uint32_t>(sp, tmweb(process_.machine.boundary(), "viewer.chain") + TrapRange::kTrapOffset);
    for (const auto value : call.args)
    {
        sp += 4;
        memory.write<std::uint32_t>(sp, value);
    }
}

// A chain's call returned here: the ret popped the return address; a cdecl call leaves its arguments for us
auto Viewer::chainStep(Call& call) -> Outcome
{
    auto& context = call.context();
    auto& stack   = chains_[process_.thread().id];
    if (stack.empty())
    {
        return Halt{ "viewer.chain with no chain running" };
    }
    auto&      chain   = stack.back();
    const auto running = std::move(chain.running);
    if (running.convention == Convention::Cdecl)
    {
        context.r[kEsp] += 4 * static_cast<std::uint32_t>(running.args.size());
    }
    const auto eax  = context.r[kEax];
    auto       next = running.done ? running.done(eax) : Next{};
    if (next.error)
    {
        return Halt{ std::format("viewer: {}", *next.error) };
    }
    for (auto then = next.then.rbegin(); then != next.then.rend(); ++then)
    {
        chain.calls.push_front(std::move(*then));
    }
    stageNext(context);
    return Resume{};
}

auto Viewer::setup() -> std::deque<GuestCall>
{
    auto&      memory   = process_.machine.memory();
    auto&      boundary = process_.machine.boundary();
    const auto base     = process_.viewer.base;
    const auto blob     = [this, &memory](const Guid& guid)
    {
        const auto address = process_.allocate(16);
        memory.write(address, guid);
        return address;
    };
    const auto expect = [](const std::uint32_t want, std::string what)
    {
        return [want, what = std::move(what)](const std::uint32_t eax) -> Next
        {
            if (eax != want)
            {
                return { std::format("{} returned {:#x}", what, eax), {} };
            }
            return {};
        };
    };
    const auto log = [](std::string text)
    {
        return [text = std::move(text)](const std::uint32_t eax) -> Next
        {
            host::log(std::format("viewer: {} = {:#x}", text, eax));
            return {};
        };
    };
    out_ = process_.allocate(16);

    auto calls = std::deque<GuestCall>{};
    // The POL1 stub decompresses .text, applies its hidden .text relocations and runs the CRT's DllMain
    calls.push_back({ "polcore DllMain", process_.viewer.entry, { base, 1, 0 }, Convention::Stdcall, [this](const std::uint32_t eax) -> Next
                      {
                          if (eax != 1)
                          {
                              return { std::format("polcore DllMain returned {:#x}", eax), {} };
                          }
                          if (keygen_)
                          {
                              patchKeygen();
                          }
                          return {};
                      } });
    calls.push_back({ "DllGetClassObject", findExport(memory, base, "DllGetClassObject"), { blob(kCoreClass), blob(kClassFactory), out_ }, Convention::Stdcall, expect(0, "polcore DllGetClassObject") });
    // The core object: CreateInstance generates its RSA key pair, the longest part of the set-up
    calls.push_back({ .make = [this, &memory, blob]
                      {
                          const auto factory = memory.read<std::uint32_t>(out_);
                          return GuestCall{ "IClassFactory::CreateInstance", memory.read<std::uint32_t>(memory.read<std::uint32_t>(factory) + 4 * kCreateInstance), { factory, 0, blob(kCoreInterface), out_ + 4 }, Convention::Stdcall, [this, &memory](const std::uint32_t eax) -> Next
                                            {
                                                core_ = memory.read<std::uint32_t>(out_ + 4);
                                                if (eax != 0 || !core_)
                                                {
                                                    return { std::format("polcore CreateInstance returned {:#x}", eax), {} };
                                                }
                                                return {};
                                            } };
                      } });
    const auto slot = [this, &memory](const unsigned index, std::vector<std::uint32_t> args, std::function<Next(std::uint32_t)> done)
    {
        return GuestCall{ .make = [this, &memory, index, args = std::move(args), done = std::move(done)]
                          {
                              auto all = std::vector<std::uint32_t>{ core_ };
                              all.insert(all.end(), args.begin(), args.end());
                              return GuestCall{ std::format("core slot {}", index), memory.read<std::uint32_t>(memory.read<std::uint32_t>(core_) + 4 * index), all, Convention::Stdcall, done };
                          } };
    };
    calls.push_back(slot(kSetAreaCode, { 1 }, expect(0, "SetAreaCode")));
    calls.push_back(slot(kSetParamInit, { process_.game.base, string(kCommandLine) }, expect(0, "SetParamInit")));
    calls.push_back(slot(kGetCommonTable, { out_ + 8 }, [this, &memory](const std::uint32_t eax) -> Next
                         {
                             table_ = memory.read<std::uint32_t>(out_ + 8);
                             if (eax != 0 || table_ != rva(kCommonTable))
                             {
                                 return { std::format("unsupported Viewer common table at {:#x}", table_), {} };
                             }
                             return {};
                         }));
    const auto entry = [this, &memory](const unsigned index, std::vector<std::uint32_t> args, std::function<Next(std::uint32_t)> done = {})
    {
        return GuestCall{ .make = [this, &memory, index, args = std::move(args), done = std::move(done)]
                          {
                              return GuestCall{ std::format("table {}", index), memory.read<std::uint32_t>(table_ + 4 * index), args, Convention::Cdecl, done };
                          } };
    };
    calls.push_back(entry(kSetRegistryLanguage, { 1 }));
    calls.push_back(entry(kSetUiLanguage, { 1 }));
    calls.push_back(entry(kSetRegistryRoot, { string(kRegistryRoot) }));
    calls.push_back(entry(kSetInstallDirectory, { string(process_.installRoot) }));
    calls.push_back(entry(kFileLayerStatus, {}, log("file layer")));
    // The game caches the account identity, so the Viewer must know it first
    const auto account = process_.allocate(16);
    calls.push_back(entry(kEncodeAccount, { account, string(login_.polId) }));
    calls.push_back(entry(kSetAccount, { account }));
    const auto internal = [this](const char* name, const std::uint32_t offset, std::vector<std::uint32_t> args, std::function<Next(std::uint32_t)> done = {})
    {
        return GuestCall{ name, rva(offset), std::move(args), Convention::Cdecl, std::move(done) };
    };
    calls.push_back(internal("InitializeInet", kInitializeInet, {}, log("INET initialization")));
    const auto endpoint = process_.allocate(kEndpointSize);
    calls.push_back(internal("ResolveAddress", kResolveAddress, { string(login_.server), endpoint }, [this, &memory, &boundary, endpoint, internal](std::uint32_t) -> Next
                             {
                                 // The profile port, in host order as tmloader writes it; then the profile client
                                 memory.write<std::uint16_t>(endpoint + 2, login_.profilePort);
                                 const auto id = string(login_.polId), password = string(login_.password);
                                 auto       next = Next{};
                                 if (!memory.read<std::uint32_t>(rva(kProfileInitialized)))
                                 {
                                     next.then.push_back(internal("InitializeProfile", kInitializeProfile, { endpoint, id, password }));
                                 }
                                 else
                                 {
                                     next.then.push_back(internal("SetProfileLogin", kSetProfileLogin, { id, password }));
                                 }
                                 next.then.push_back(internal("SetProfileEndpoint", kSetProfileEndpoint, { endpoint }, [this, &memory, endpoint](std::uint32_t) -> Next
                                                              {
                                                                  auto copy = std::array<std::uint8_t, kEndpointSize>{};
                                                                  memory.read(endpoint, copy);
                                                                  memory.write(rva(kProfileEndpoint), copy);
                                                                  return {};
                                                              }));
                                 next.then.push_back(internal("ProfileFollowUp", kProfileFollowUp, {}, [this, &memory, &boundary](std::uint32_t) -> Next
                                                              {
                                                                  // tmloader's hooks: keep the table as set up, then point 132 and 133 at ours
                                                                  original_.resize(kTableEntries);
                                                                  for (auto i = 0u; i < kTableEntries; ++i)
                                                                  {
                                                                      original_[i] = memory.read<std::uint32_t>(table_ + 4 * i);
                                                                  }
                                                                  memory.write<std::uint32_t>(table_ + 4 * kGameIrcOpen, tmweb(boundary, "viewer.irc_open"));
                                                                  memory.write<std::uint32_t>(table_ + 4 * kGameIrcOpenCheck, tmweb(boundary, "viewer.irc_check"));
                                                                  host::log("viewer: polcore is set up");
                                                                  return {};
                                                              }));
                                 return next;
                             }));
    return calls;
}

void Viewer::patchKeygen()
{
    auto&      memory   = process_.machine.memory();
    auto&      boundary = process_.machine.boundary();
    const auto jump     = [&](const std::uint32_t from, const char* to)
    {
        const auto target = tmweb(boundary, to);
        memory.write<std::uint8_t>(from, 0xE9); // jmp rel32
        memory.write<std::uint32_t>(from + 1, target - (from + 5));
    };
    jump(rva(kMontgomery), "viewer.montgomery");
    jump(rva(kAdd512), "viewer.add512");
    jump(rva(kShiftRight), "viewer.shift_right512");
}

// Game IRC open: the server and the account replace what TM passes, then polcore's own entry runs
auto Viewer::ircOpen(Call& call) -> Outcome
{
    auto&      memory  = process_.machine.memory();
    auto&      context = call.context();
    const auto esp     = context.r[kEsp];
    host::log(std::format("viewer: Game IRC open; version {} mode {}", call.argString(3), call.arg(4)));
    memory.write<std::uint32_t>(esp + 4, string(login_.server));
    memory.write<std::uint32_t>(esp + 8, string(login_.polId));
    memory.write<std::uint32_t>(esp + 12, string(login_.password));
    context.eip = original_[kGameIrcOpen];
    return Resume{};
}

// Game IRC open check: once it first reports the login, seed the profile cipher and load the profile before TM sees
// the result
auto Viewer::ircCheck(Call& call) -> Outcome
{
    auto& context = call.context();
    if (loaded_)
    {
        context.eip = original_[kGameIrcOpenCheck];
        return Resume{};
    }
    const auto slot = call.arg(0);
    const auto back = call.returnAddress();
    context.r[kEsp] += 4; // pop the return address: the chain returns to TM itself
    auto result = std::make_shared<std::uint32_t>();
    start({ GuestCall{ "Game IRC open check", original_[kGameIrcOpenCheck], { slot }, Convention::Cdecl, [this, slot, result](const std::uint32_t eax) -> Next
                       {
                           *result = eax;
                           if (eax != 1 || loaded_)
                           {
                               return {};
                           }
                           loaded_ = true;
                           host::log("viewer: Game IRC login accepted; loading the profile");
                           return { {}, profileBootstrap(slot) };
                       } } },
          back,
          [result]
          {
              return *result;
          });
    return Resume{};
}

// tmloader's seedProfile and loadProfile: the Game IRC slot's cipher context and its matrix outlive the slot
auto Viewer::profileBootstrap(const std::uint32_t slot) -> std::vector<GuestCall>
{
    auto&      memory = process_.machine.memory();
    const auto irc    = rva(kGameIrcSlots) + slot * kGameIrcSlotSize;
    const auto cipher = process_.allocate(kCipherSize), matrix = process_.allocate(kCipherMatrixSize);
    auto       bytes = std::vector<std::uint8_t>(kCipherSize);
    memory.read(irc + kGameIrcCipher, bytes);
    memory.write(cipher, std::span<const std::uint8_t>{ bytes });
    bytes.resize(kCipherMatrixSize);
    memory.read(memory.read<std::uint32_t>(cipher + kCipherMatrixPointer), bytes);
    memory.write(matrix, std::span<const std::uint8_t>{ bytes });
    memory.write<std::uint32_t>(cipher + kCipherMatrixPointer, matrix);
    const auto keys = std::vector<std::uint32_t>{ cipher, memory.read<std::uint32_t>(irc + kGameIrcCipherKeys), memory.read<std::uint32_t>(irc + kGameIrcCipherKeys + 4) };

    auto calls = std::vector<GuestCall>{};
    calls.push_back({ "SeedProfileCipher", rva(kSeedProfileCipher), keys });
    calls.push_back({ "SeedFollowUp", rva(kSeedFollowUp), {} });
    for (const auto request : { kLoadHandle, kLoadCharacter, kLoadStatus })
    {
        calls.push_back({ std::format("profile request {}", static_cast<unsigned>(request)), original_[request], {}, Convention::Cdecl, [this, request](const std::uint32_t ticket) -> Next
                          {
                              return { {}, { poll(request, ticket, process_.machine.scheduler().now()) } };
                          } });
    }
    return calls;
}

// One check of a profile ticket; while it reports 0, Sleep and check again, for up to five seconds
auto Viewer::poll(const unsigned request, const std::uint32_t ticket, const std::uint64_t started) -> GuestCall
{
    return GuestCall{ std::format("profile check {}", request + 1), original_[request + 1], { ticket }, Convention::Cdecl, [this, request, ticket, started](const std::uint32_t status) -> Next
                      {
                          const auto waited = process_.machine.scheduler().now() - started;
                          if (static_cast<std::int32_t>(ticket) >= 0 && status == 0 && waited < kProfileTimeoutMs)
                          {
                              const auto sleep = process_.machine.boundary().resolve("kernel32.dll", "Sleep");
                              return { {}, { GuestCall{ "Sleep", sleep, { kPollMs }, Convention::Stdcall }, poll(request, ticket, started) } };
                          }
                          results_[request] = status;
                          host::log(std::format("viewer: profile request {} ticket {} result {}", request, static_cast<std::int32_t>(ticket), static_cast<std::int32_t>(status)));
                          return {};
                      } };
}

auto Viewer::probe() -> std::deque<GuestCall>
{
    auto&      memory    = process_.machine.memory();
    auto&      boundary  = process_.machine.boundary();
    const auto callback  = tmweb(boundary, "viewer.callback");
    const auto callbacks = process_.allocate(28);
    // As TM's block at 0x1023FFA0: two receive handlers twice each, then -1, 0, 1
    for (const auto [index, value] : { std::pair{ 0u, callback }, { 1u, callback }, { 2u, callback }, { 3u, callback }, { 4u, 0xFFFFFFFFu }, { 5u, 0u }, { 6u, 1u } })
    {
        memory.write<std::uint32_t>(callbacks + 4 * index, value);
    }
    const auto cipher = process_.allocate(0x1000), queue = process_.allocate(0x1000);
    const auto started = std::make_shared<std::uint64_t>();
    // Open check, then the poll and a Sleep while it reports 0
    const auto check = std::make_shared<std::function<GuestCall(std::uint32_t)>>();
    *check           = [this, &memory, &boundary, started, check](const std::uint32_t slot)
    {
        return GuestCall{ .make = [this, &memory, &boundary, started, check, slot]
                          {
                              return GuestCall{ "Game IRC open check (hooked)", memory.read<std::uint32_t>(table_ + 4 * kGameIrcOpenCheck), { slot }, Convention::Cdecl, [this, &memory, &boundary, started, check, slot](const std::uint32_t eax) -> Next
                                                {
                                                    if (eax == 1)
                                                    {
                                                        return {};
                                                    }
                                                    if (eax != 0 || process_.machine.scheduler().now() - *started > kProbeTimeoutMs)
                                                    {
                                                        return { std::format("Game IRC open check returned {:#x}", eax), {} };
                                                    }
                                                    const auto sleep = boundary.resolve("kernel32.dll", "Sleep");
                                                    return { {}, { GuestCall{ "Game IRC poll", memory.read<std::uint32_t>(table_ + 4 * kGameIrcPoll), { slot } }, GuestCall{ "Sleep", sleep, { kPollMs }, Convention::Stdcall }, (*check)(slot) } };
                                                } };
                          } };
    };
    auto calls = std::deque<GuestCall>{};
    calls.push_back({ .make = [this, &memory, callbacks, cipher, queue, started, check]
                      {
                          *started = process_.machine.scheduler().now();
                          return GuestCall{ "Game IRC open (hooked)", memory.read<std::uint32_t>(table_ + 4 * kGameIrcOpen), { string("gi000.pol.com"), string(""), string(""), string(kClientLabel), 8, callbacks, cipher, queue, 0x100 }, Convention::Cdecl, [check](const std::uint32_t slot) -> Next
                                            {
                                                if (static_cast<std::int32_t>(slot) < 0)
                                                {
                                                    return { std::format("Game IRC open returned {}", static_cast<std::int32_t>(slot)), {} };
                                                }
                                                return { {}, { (*check)(slot) } };
                                            } };
                      } });
    return calls;
}

auto Viewer::core() const -> std::uint32_t
{
    return core_;
}

auto Viewer::profileLoaded() const -> bool
{
    return loaded_;
}

auto Viewer::profileResults() const -> const std::map<unsigned, std::uint32_t>&
{
    return results_;
}

} // namespace tmweb::hle
