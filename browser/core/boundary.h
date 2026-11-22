#pragma once

// The boundary between guest code and host stand-ins.
//
// Every stand-in is a function slot in a synthetic module image (kernel32.dll, user32.dll, ...) or a COM vtable.
// A slot is 32 bytes: a stack-neutral prologue the game's protector can copy from (mov edi,edi; push ebp;
// mov ebp,esp; pop ebp; nops), then the trap at offset 16. When eip reaches a trap the CPU stops, the boundary calls
// the C++ handler with a Call, and then completes the call the way the guest expects: EAX/EDX from the Return,
// the return address popped, and for stdcall the arguments too.
//
// Synthetic modules are real PE images in guest memory with export directories, because the protector finds
// kernel32 by scanning for MZ and walks its exports itself.

#include "core/cpu.h"
#include "core/threads.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tmweb
{

class Machine;

enum class Convention : std::uint8_t
{
    Stdcall,  // callee pops; WINAPI and COM methods (COM: argument 0 is this)
    Cdecl,    // caller pops; the Viewer's common-function table, CRT callbacks
    Thiscall, // this in ECX, callee pops the stack arguments
};

// Finish the call: EAX and EDX, then return to the caller
struct Return
{
    std::uint32_t eax = 0;
    std::uint32_t edx = 0;
};

// Park the calling thread until the wait is satisfied or times out; resume then gives the call's result
struct Block
{
    Wait                                     wait;
    std::function<Return(const WaitResult&)> resume;
};

// The handler set eip/esp itself (RaiseException, ExitThread, a jump into guest code); nothing is popped
struct Resume
{
};

// Stop the machine: an unimplemented import, a milestone reached, a fatal error
struct Halt
{
    std::string reason;
};

using Outcome = std::variant<Return, Block, Resume, Halt>;

class Call final
{
public:
    Call(Machine& machine, Context& context, std::string_view name);

    // Stack arguments after the return address. For Thiscall, ECX is self() and arg(0) is the first stack argument.
    auto arg(unsigned index) const -> std::uint32_t;
    auto self() const -> std::uint32_t;
    auto argString(unsigned index) const -> std::string;        // empty for a null pointer
    auto argWideString(unsigned index) const -> std::u16string; // empty for a null pointer
    auto returnAddress() const -> std::uint32_t;
    auto name() const -> std::string_view; // "kernel32.dll!CreateFileA"

    auto machine() const -> Machine&;
    auto memory() const -> Memory&;
    auto context() const -> Context&;

private:
    Machine&         machine_;
    Context&         context_;
    std::string_view name_;
};

using Handler = std::function<Outcome(Call&)>;

struct Function
{
    std::string  module; // lower case, "kernel32.dll"
    std::string  name;   // "CreateFileA", or "#11" for an ordinal
    Convention   convention = Convention::Stdcall;
    std::uint8_t argc       = 0; // stack arguments; for COM methods including this
    Handler      handler;        // empty: an import nobody implements yet; calling it halts
};

struct Method
{
    std::string  name;
    std::uint8_t argc = 1; // including this
    Handler      handler;
};

class Boundary final
{
public:
    explicit Boundary(Memory& memory);

    // Register stand-ins before publish(); each gets an export in its module's image
    void add(Function function);
    // Build the synthetic module images and their export directories in guest memory
    void publish();

    // The module's image base, or nothing for a module no stand-in names
    auto module(std::string_view name) const -> std::optional<std::uint32_t>;
    // Entry point for module!name. After publish() an unknown name gets a fresh slot whose handler halts with
    // "unhandled import", so GetProcAddress and the protector's own resolution always get an address.
    auto resolve(std::string_view module, std::string_view name) -> std::uint32_t;
    // The function behind an entry point (slot start, or anywhere inside the slot's prologue)
    auto find(std::uint32_t address) const -> const Function*;

    // A COM interface: a guest vtable of stdcall slots, one per method, in order
    auto addInterface(std::string_view name, std::span<const Method> methods) -> std::uint32_t;
    // A guest object for the host: { vtable, cookie }, 8 bytes. The cookie finds the host side again.
    auto newObject(std::uint32_t vtable, std::shared_ptr<void> host) -> std::uint32_t;

    template <class T>
    auto object(const std::uint32_t guestObject) const -> T*
    {
        return static_cast<T*>(objectHost(guestObject));
    }

    // The slot whose trap returns from a host-to-guest call (Machine::callGuest)
    auto returnTrap() const -> std::uint32_t;

    auto traps() const -> TrapRange;
    // Run the handler behind a trap and complete the call; the Machine calls this on Stop::Trap
    auto dispatch(Machine& machine, Context& context, std::uint32_t trap) -> Outcome;

private:
    auto newSlot(Function function) -> std::uint32_t;
    auto objectHost(std::uint32_t guestObject) const -> void*;

    Memory&                              memory_;
    std::vector<Function>                slots_;   // by slot index
    std::map<std::string, std::uint32_t> entries_; // "module!name" -> entry point
    std::map<std::string, std::uint32_t> modules_; // module -> image base
    std::vector<std::shared_ptr<void>>   objects_; // by cookie
    std::uint32_t                        next_      = layout::kModules;
    bool                                 published_ = false;
};

} // namespace tmweb
