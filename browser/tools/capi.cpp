// A C interface to the interpreter alone (memory, CPU, one context), for the Python differential test against
// Unicorn (tools/difftest.py). Native builds only.
#include "core/cpu.h"
#include "core/memory.h"

#include <cstring>
#include <exception>
#include <memory>

#if defined(_WIN32)
#define TMWEB_EXPORT extern "C" __declspec(dllexport)
#else
#define TMWEB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{

struct Instance
{
    tmweb::Memory  memory;
    tmweb::Cpu     cpu{ memory };
    tmweb::Context context;
};

} // namespace

// The register file as Python sees it; physical x87 registers R0..R7, 10 bytes each in memory order
struct TmwebState
{
    std::uint32_t r[8];
    std::uint32_t eip;
    std::uint32_t eflags;
    std::uint32_t fsBase;
    std::uint32_t gsBase;
    std::uint16_t control;
    std::uint16_t status;
    std::uint16_t tag;
    std::uint16_t opcode;
    std::uint32_t lastInstruction;
    std::uint32_t lastOperand;
    std::uint8_t  st[8][10];
};

struct TmwebEvent
{
    std::uint32_t stop;
    std::uint32_t address;
    std::uint32_t vector;
    std::uint32_t faultAddress;
    std::uint32_t faultAccess;
};

TMWEB_EXPORT auto tmweb_cpu_create() -> void*
{
    try
    {
        return new Instance{};
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

TMWEB_EXPORT void tmweb_cpu_destroy(void* const instance)
{
    delete static_cast<Instance*>(instance);
}

TMWEB_EXPORT auto tmweb_cpu_map(void* const instance, const std::uint32_t address, const std::uint32_t size) -> int
{
    try
    {
        static_cast<Instance*>(instance)->memory.map(address, size);
        return 0;
    }
    catch (const std::exception&)
    {
        return -1;
    }
}

TMWEB_EXPORT auto tmweb_cpu_write(void* const instance, const std::uint32_t address, const std::uint8_t* const data, const std::uint32_t size) -> int
{
    try
    {
        static_cast<Instance*>(instance)->memory.write(address, std::span{ data, size });
        return 0;
    }
    catch (const tmweb::GuestFault&)
    {
        return -1;
    }
}

TMWEB_EXPORT auto tmweb_cpu_read(void* const instance, const std::uint32_t address, std::uint8_t* const out, const std::uint32_t size) -> int
{
    try
    {
        static_cast<Instance*>(instance)->memory.read(address, std::span{ out, size });
        return 0;
    }
    catch (const tmweb::GuestFault&)
    {
        return -1;
    }
}

TMWEB_EXPORT void tmweb_cpu_get(void* const instance, TmwebState* const out)
{
    const auto& c = static_cast<Instance*>(instance)->context;
    std::memcpy(out->r, c.r.data(), sizeof(out->r));
    out->eip             = c.eip;
    out->eflags          = c.eflags;
    out->fsBase          = c.fsBase;
    out->gsBase          = c.gsBase;
    out->control         = c.fpu.control;
    out->status          = c.fpu.status;
    out->tag             = c.fpu.tag;
    out->opcode          = c.fpu.lastOpcode;
    out->lastInstruction = c.fpu.lastInstruction;
    out->lastOperand     = c.fpu.lastOperand;
    for (auto index = 0; index < 8; ++index)
    {
        std::memcpy(out->st[index], &c.fpu.st[index].significand, 8);
        std::memcpy(out->st[index] + 8, &c.fpu.st[index].signExponent, 2);
    }
}

TMWEB_EXPORT void tmweb_cpu_set(void* const instance, const TmwebState* const in)
{
    auto& c = static_cast<Instance*>(instance)->context;
    std::memcpy(c.r.data(), in->r, sizeof(in->r));
    c.eip                 = in->eip;
    c.eflags              = in->eflags;
    c.fsBase              = in->fsBase;
    c.gsBase              = in->gsBase;
    c.fpu.control         = in->control;
    c.fpu.status          = in->status;
    c.fpu.tag             = in->tag;
    c.fpu.lastOpcode      = in->opcode;
    c.fpu.lastInstruction = in->lastInstruction;
    c.fpu.lastOperand     = in->lastOperand;
    for (auto index = 0; index < 8; ++index)
    {
        std::memcpy(&c.fpu.st[index].significand, in->st[index], 8);
        std::memcpy(&c.fpu.st[index].signExponent, in->st[index] + 8, 2);
    }
}

TMWEB_EXPORT void tmweb_cpu_traps(void* const instance, const std::uint32_t base, const std::uint32_t size)
{
    static_cast<Instance*>(instance)->cpu.setTraps(tmweb::TrapRange{ base, size });
}

// Run up to count instructions; returns the stop kind (tmweb::Stop)
TMWEB_EXPORT auto tmweb_cpu_run(void* const instance, const std::uint32_t count, TmwebEvent* const out) -> std::uint32_t
{
    auto&      self   = *static_cast<Instance*>(instance);
    const auto event  = self.cpu.run(self.context, count);
    out->stop         = static_cast<std::uint32_t>(event.stop);
    out->address      = event.address;
    out->vector       = event.vector;
    out->faultAddress = event.fault.address;
    out->faultAccess  = static_cast<std::uint32_t>(event.fault.access);
    return out->stop;
}

TMWEB_EXPORT auto tmweb_cpu_retired(void* const instance) -> std::uint64_t
{
    return static_cast<Instance*>(instance)->cpu.retired();
}

// Run until eip reaches address (not executed), a trap, a fault or an interrupt, or max instructions; returns the
// stop kind, with Budget meaning max ran out and the address was not reached
TMWEB_EXPORT auto tmweb_cpu_run_until(void* const instance, const std::uint32_t address, const std::uint32_t max, TmwebEvent* const out) -> std::uint32_t
{
    auto& self = *static_cast<Instance*>(instance);
    for (auto count = std::uint32_t{}; count < max; ++count)
    {
        if (self.context.eip == address)
        {
            out->stop    = 0xFF;
            out->address = address;
            return out->stop;
        }
        const auto event = self.cpu.run(self.context, 1);
        if (event.stop != tmweb::Stop::Budget)
        {
            out->stop         = static_cast<std::uint32_t>(event.stop);
            out->address      = event.address;
            out->vector       = event.vector;
            out->faultAddress = event.fault.address;
            out->faultAccess  = static_cast<std::uint32_t>(event.fault.access);
            return out->stop;
        }
    }
    out->stop = static_cast<std::uint32_t>(tmweb::Stop::Budget);
    return out->stop;
}
