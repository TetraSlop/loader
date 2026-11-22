#include "tests/unicorn_runner.h"

#include <unicorn/unicorn.h>

#include <array>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>

namespace tmweb::test
{
namespace
{

// A GDT outside the guest arena, in Unicorn only: flat code and data, and FS on the current TEB
constexpr std::uint32_t kGdt   = 0x7FFE0000;
constexpr int           kCode  = 0x08;
constexpr int           kData  = 0x10;
constexpr int           kFs    = 0x18;
constexpr auto          kPages = 0x100000u;
constexpr std::uint64_t kSlice = 10000; // basic blocks a thread runs before the others get a turn

auto descriptor(const std::uint32_t base, const std::uint32_t limit, const std::uint8_t access, const std::uint8_t flags)
    -> std::uint64_t
{
    return (limit & 0xFFFFull) | (std::uint64_t{ base & 0xFFFFFF } << 16) | (std::uint64_t{ access } << 40) |
           (std::uint64_t{ (limit >> 16) & 0xF } << 48) | (std::uint64_t{ flags } << 52) | (std::uint64_t{ base >> 24 } << 56);
}

void check(const uc_err error, const char* what)
{
    if (error != UC_ERR_OK)
    {
        throw std::runtime_error(std::format("{}: {}", what, uc_strerror(error)));
    }
}

constexpr auto kRegs = std::array<int, 8>{ UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };

} // namespace

struct Hooks
{
    static auto unmapped(uc_engine* uc, const uc_mem_type type, const std::uint64_t address, int, std::int64_t, void* user) -> bool
    {
        auto&      self   = *static_cast<UnicornRunner*>(user);
        auto&      memory = self.machine_.memory();
        const auto page   = static_cast<std::uint32_t>(address >> 12);
        if (address < 0x100000000ull && memory.mapped(page << 12) && !self.shared_[page])
        {
            // Share the whole run of tmweb pages around it that Unicorn does not have yet
            auto first = page;
            while (first > 0 && memory.mapped((first - 1) << 12) && !self.shared_[first - 1])
            {
                --first;
            }
            auto last = page;
            while (last + 1 < kPages && memory.mapped((last + 1) << 12) && !self.shared_[last + 1])
            {
                ++last;
            }
            const auto size = (last - first + 1) << 12;
            check(uc_mem_map_ptr(uc, std::uint64_t{ first } << 12, size, UC_PROT_ALL, memory.host(first << 12)), "uc_mem_map_ptr");
            for (auto p = first; p <= last; ++p)
            {
                self.shared_[p] = true;
            }
            return true;
        }
        const auto access = type == UC_MEM_WRITE_UNMAPPED ? Access::Write : type == UC_MEM_FETCH_UNMAPPED ? Access::Execute
                                                                                                          : Access::Read;
        self.event_       = Event{ Stop::Fault, 0, 0, GuestFault{ static_cast<std::uint32_t>(address), access } };
        self.stopped_     = true;
        return false;
    }

    static void code(uc_engine* uc, const std::uint64_t address, std::uint32_t, void* user)
    {
        auto& self = *static_cast<UnicornRunner*>(user);
        if (self.machine_.boundary().traps().contains(static_cast<std::uint32_t>(address)))
        {
            self.event_   = Event{ Stop::Trap, static_cast<std::uint32_t>(address) };
            self.stopped_ = true;
            uc_emu_stop(uc);
        }
    }

    static void block(uc_engine* uc, std::uint64_t, std::uint32_t, void* user)
    {
        auto& self = *static_cast<UnicornRunner*>(user);
        if (++self.blocks_ > self.limit_ && !self.stopped_)
        {
            self.event_   = Event{ Stop::Budget };
            self.stopped_ = true;
            uc_emu_stop(uc);
        }
    }

    static void interrupt(uc_engine* uc, const std::uint32_t vector, void* user)
    {
        auto& self    = *static_cast<UnicornRunner*>(user);
        self.event_   = Event{ Stop::Interrupt, 0, vector };
        self.stopped_ = true;
        uc_emu_stop(uc);
    }
};

UnicornRunner::UnicornRunner(Machine& machine)
: machine_(machine)
, shared_(kPages, false)
{
    check(uc_open(UC_ARCH_X86, UC_MODE_32, &uc_), "uc_open");
    check(uc_mem_map(uc_, kGdt, 0x1000, UC_PROT_READ | UC_PROT_WRITE), "uc_mem_map");
    const auto gdt = std::array<std::uint64_t, 4>{ 0, descriptor(0, 0xFFFFF, 0x9A, 0xC), descriptor(0, 0xFFFFF, 0x92, 0xC), 0 };
    check(uc_mem_write(uc_, kGdt, gdt.data(), sizeof(gdt)), "uc_mem_write");
    auto table = uc_x86_mmr{ 0, kGdt, 0x1F, 0 };
    check(uc_reg_write(uc_, UC_X86_REG_GDTR, &table), "GDTR");
    auto selector = kCode;
    check(uc_reg_write(uc_, UC_X86_REG_CS, &selector), "CS");
    selector = kData;
    for (const auto reg : { UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_GS })
    {
        check(uc_reg_write(uc_, reg, &selector), "segment");
    }

    uc_hook hook{};
    check(uc_hook_add(uc_, &hook, UC_HOOK_MEM_UNMAPPED, reinterpret_cast<void*>(&Hooks::unmapped), this, 1, 0), "hook");
    // Over every address, not just the traps: with a code hook on an instruction Unicorn keeps eip exact, so a
    // fault reports the faulting instruction rather than the start of its block
    check(uc_hook_add(uc_, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(&Hooks::code), this, 1, 0), "hook");
    check(uc_hook_add(uc_, &hook, UC_HOOK_BLOCK, reinterpret_cast<void*>(&Hooks::block), this, 1, 0), "hook");
    check(uc_hook_add(uc_, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&Hooks::interrupt), this, 1, 0), "hook");
}

UnicornRunner::~UnicornRunner()
{
    uc_close(uc_);
}

auto UnicornRunner::run(Context& context, const std::uint64_t budget) -> Event
{
    if (context.fsBase != fsBase_)
    {
        const auto fs = descriptor(context.fsBase, 0xFFF, 0x92, 0x4);
        check(uc_mem_write(uc_, kGdt + kFs, &fs, sizeof(fs)), "uc_mem_write");
        auto selector = kFs;
        check(uc_reg_write(uc_, UC_X86_REG_FS, &selector), "FS");
        fsBase_ = context.fsBase;
    }
    for (auto i = 0u; i < kRegs.size(); ++i)
    {
        uc_reg_write(uc_, kRegs[i], &context.r[i]);
    }
    uc_reg_write(uc_, UC_X86_REG_EFLAGS, &context.eflags);
    auto word = std::uint32_t{ context.fpu.control };
    uc_reg_write(uc_, UC_X86_REG_FPCW, &word);

    stopped_     = false;
    event_       = Event{};
    blocks_      = 0;
    limit_       = budget;
    const auto e = uc_emu_start(uc_, context.eip, 0, 0, 0);

    for (auto i = 0u; i < kRegs.size(); ++i)
    {
        uc_reg_read(uc_, kRegs[i], &context.r[i]);
    }
    uc_reg_read(uc_, UC_X86_REG_EIP, &context.eip);
    uc_reg_read(uc_, UC_X86_REG_EFLAGS, &context.eflags);
    uc_reg_read(uc_, UC_X86_REG_FPCW, &word);
    context.fpu.control = static_cast<std::uint16_t>(word);
    if (!stopped_)
    {
        // An error with no hook behind it: an instruction Unicorn cannot decode, or a protection fault
        event_ = e == UC_ERR_OK ? Event{ Stop::Halt } : Event{ Stop::Interrupt, 0, 6 };
    }
    event_.address = context.eip;
    return event_;
}

// Machine::run's loop: round-robin slices through the scheduler, parked threads woken by it
auto UnicornRunner::runUntilHalt(const std::uint64_t budget) -> RunResult
{
    auto       result    = RunResult{};
    auto&      boundary  = machine_.boundary();
    auto&      scheduler = machine_.scheduler();
    auto       remaining = budget;
    const auto halt      = [&](std::string reason)
    {
        result.halted = true;
        result.reason = std::move(reason);
        return result;
    };
    while (remaining > 0)
    {
        if (!scheduler.schedule())
        {
            if (scheduler.waiting() == 0 || !scheduler.nextDeadline())
            {
                return halt("every guest thread waits on something nobody will signal");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
            continue;
        }
        auto&      thread  = scheduler.current();
        auto&      context = thread.context;
        const auto event   = run(context, std::min(remaining, kSlice));
        remaining -= std::min(remaining, blocks_);
        result.instructions += blocks_;
        try
        {
            switch (event.stop)
            {
                case Stop::Trap:
                {
                    auto outcome = boundary.dispatch(machine_, context, event.address);
                    if (const auto* done = std::get_if<Return>(&outcome))
                    {
                        machine_.complete(context, event.address, *done);
                    }
                    else if (auto* block = std::get_if<Block>(&outcome))
                    {
                        scheduler.block(std::move(block->wait), [this, &thread, trap = event.address, resume = std::move(block->resume)](const WaitResult& woken)
                                        {
                                            machine_.complete(thread.context, trap, resume(woken));
                                        });
                    }
                    else if (const auto* stop = std::get_if<Halt>(&outcome))
                    {
                        return halt(stop->reason);
                    }
                    break;
                }
                case Stop::Fault:
                {
                    const auto kind        = event.fault.access == Access::Write ? 1u : event.fault.access == Access::Execute ? 8u
                                                                                                                              : 0u;
                    const auto information = std::array<std::uint32_t, 2>{ kind, event.fault.address };
                    ++result.exceptions;
                    if (!machine_.raise(kAccessViolation, context.eip, information))
                    {
                        return halt(std::format("unhandled access violation at {:#x} touching {:#x}", context.eip, event.fault.address));
                    }
                    break;
                }
                case Stop::Interrupt:
                {
                    // As Machine::exception: Unicorn leaves eip after a software interrupt, at a fault otherwise
                    auto code    = std::uint32_t{ kAccessViolation };
                    auto address = context.eip;
                    switch (event.vector)
                    {
                        case 0:
                            code = kIntegerDivide;
                            break;
                        case 1:
                            code = kSingleStep;
                            context.eflags &= ~kTF;
                            break;
                        case 3:
                            code        = kBreakpoint;
                            context.eip = context.eip - 1;
                            address     = context.eip;
                            break;
                        case 4:
                            code = kIntegerOverflow;
                            break;
                        case 6:
                            code = kIllegalInstruction;
                            break;
                        case 0x2D:
                            code = kBreakpoint;
                            break;
                        default:
                            context.eip -= 2; // INT n: a general protection fault at the instruction
                            address = context.eip;
                            break;
                    }
                    ++result.exceptions;
                    if (!machine_.raise(code, address, {}))
                    {
                        return halt(std::format("unhandled exception {:#x} at {:#x}", code, address));
                    }
                    break;
                }
                case Stop::Budget:
                    break;
                case Stop::Halt:
                    return halt(std::format("stopped at {:#x}", context.eip));
            }
        }
        catch (const GuestFault& fault)
        {
            // Building an exception record or completing a call touched unmapped memory
            return halt(std::format("the host faulted on {:#x} handling a stop at {:#x} on thread {}", fault.address, context.eip, thread.id));
        }
    }
    return halt("budget spent");
}

} // namespace tmweb::test
