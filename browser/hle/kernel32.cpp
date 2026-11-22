// kernel32 for TM.dll, its protector and its CRT. Paths go to the runtime folder through HostFiles; writes are
// refused until the game needs them. Text conversion follows Kestrel's compat layer (theater-fit
// kestrel/port/compat/win32/impl/kernel.cpp, commit e1240c4): narrow strings are Shift-JIS bytes that widen one byte
// to one character, so bytes round-trip, which is enough for paths and logs.
#include "hle/process.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace tmweb::hle
{
namespace
{

enum : std::uint32_t
{
    kWaitObject0     = 0,
    kWaitTimeout     = 0x102,
    kWaitFailed      = 0xFFFFFFFF,
    kGenericWrite    = 0x40000000,
    kCreateNew       = 1,
    kCreateAlways    = 2,
    kOpenExisting    = 3,
    kOpenAlways      = 4,
    kMemReserve      = 0x2000,
    kMemRelease      = 0x8000,
    kCreateSuspended = 4,
    kCodePage        = 932,
    kLocale          = 0x411,
    kHeapHandle      = 0x00150000,
    kProcessPseudo   = 0xFFFFFFFF,
    kThreadPseudo    = 0xFFFFFFFE,
    kPageExecuteRW   = 0x40,
    kLowercase       = 0x100,
    kUppercase       = 0x200,
    kIgnoreCase      = 1,
};

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

auto lowerAscii(const char c) -> char
{
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

auto lowerText(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), lowerAscii);
    return text;
}

// A module name as GetModuleHandle compares it: no folder, lower case, ".dll" when there is no extension
auto moduleName(std::string name) -> std::string
{
    if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos)
    {
        name.erase(0, slash + 1);
    }
    name = lowerText(std::move(name));
    if (name.find('.') == std::string::npos)
    {
        name += ".dll";
    }
    return name;
}

// Win32's wildcard match: '*' and '?', without case; "*.*" is everything
auto matches(const std::string_view mask, const std::string_view name) -> bool
{
    if (mask == "*.*" || mask == "*")
    {
        return true;
    }
    if (mask.empty())
    {
        return name.empty();
    }
    if (mask[0] == '*')
    {
        for (auto i = std::size_t{}; i <= name.size(); ++i)
        {
            if (matches(mask.substr(1), name.substr(i)))
            {
                return true;
            }
        }
        return false;
    }
    return !name.empty() && (mask[0] == '?' || lowerAscii(mask[0]) == lowerAscii(name[0])) && matches(mask.substr(1), name.substr(1));
}

// Text of a counted or NUL-terminated (-1) argument; -1 includes the terminator, as Win32 counts it
auto narrowArgument(const Memory& memory, const std::uint32_t address, const std::int32_t length) -> std::string
{
    if (length < 0)
    {
        auto text = memory.readString(address);
        text.push_back('\0');
        return text;
    }
    auto text = std::string(static_cast<std::size_t>(length), '\0');
    memory.read(address, std::span{ reinterpret_cast<std::uint8_t*>(text.data()), text.size() });
    return text;
}

auto wideArgument(const Memory& memory, const std::uint32_t address, const std::int32_t length) -> std::u16string
{
    if (length < 0)
    {
        auto text = memory.readWideString(address);
        text.push_back(u'\0');
        return text;
    }
    auto text = std::u16string(static_cast<std::size_t>(length), u'\0');
    memory.read(address, std::span{ reinterpret_cast<std::uint8_t*>(text.data()), 2 * text.size() });
    return text;
}

// Copy text out as Win32 does: its length alone for a capacity of 0, and 0 when it does not fit (compat's Out)
template <class Text>
auto copyOut(Process& process, const Text& text, const std::uint32_t destination, const std::uint32_t capacity) -> std::uint32_t
{
    const auto length = static_cast<std::uint32_t>(text.size());
    if (capacity == 0)
    {
        return length;
    }
    if (capacity < length)
    {
        process.setLastError(kErrorInsufficient);
        return 0;
    }
    process.machine.memory().write(destination,
                                   std::span{ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() * sizeof(typename Text::value_type) });
    return length;
}

// lstrcpyn-style: as much as fits with a terminator; returns the characters copied
auto copyString(Memory& memory, const std::uint32_t destination, const std::uint32_t capacity, const std::string_view text) -> std::uint32_t
{
    if (capacity == 0)
    {
        return 0;
    }
    const auto length = std::min<std::uint32_t>(static_cast<std::uint32_t>(text.size()), capacity - 1);
    memory.writeString(destination, text.substr(0, length));
    return length;
}

// SYSTEMTIME from a point in time; the guest's local time is UTC
void writeSystemTime(Memory& memory, const std::uint32_t address, const std::chrono::sys_time<std::chrono::milliseconds> time)
{
    const auto day     = std::chrono::floor<std::chrono::days>(time);
    const auto date    = std::chrono::year_month_day{ day };
    const auto clock   = std::chrono::hh_mm_ss{ time - day };
    const auto weekday = std::chrono::weekday{ day };
    const auto fields  = std::array<std::uint16_t, 8>{ static_cast<std::uint16_t>(static_cast<int>(date.year())),
                                                       static_cast<std::uint16_t>(static_cast<unsigned>(date.month())),
                                                       static_cast<std::uint16_t>(weekday.c_encoding()),
                                                       static_cast<std::uint16_t>(static_cast<unsigned>(date.day())),
                                                       static_cast<std::uint16_t>(clock.hours().count()),
                                                       static_cast<std::uint16_t>(clock.minutes().count()),
                                                       static_cast<std::uint16_t>(clock.seconds().count()),
                                                       static_cast<std::uint16_t>(clock.subseconds().count()) };
    for (auto i = 0u; i < fields.size(); ++i)
    {
        memory.write<std::uint16_t>(address + 2 * i, fields[i]);
    }
}

// Character type bits for GetStringType's CT_CTYPE1; characters past ASCII report none
auto characterType(const std::uint32_t c) -> std::uint16_t
{
    if (c >= 0x80)
    {
        return 0;
    }
    auto type = std::uint16_t{};
    if (c >= 'A' && c <= 'Z')
    {
        type |= 0x101; // upper, alpha
    }
    if (c >= 'a' && c <= 'z')
    {
        type |= 0x102; // lower, alpha
    }
    if (c >= '0' && c <= '9')
    {
        type |= 0x84; // digit, hex digit
    }
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
    {
        type |= 0x80;
    }
    if (c == ' ' || (c >= 0x09 && c <= 0x0D))
    {
        type |= 0x08;
    }
    if (c == ' ' || c == '\t')
    {
        type |= 0x40;
    }
    if (c < 0x20 || c == 0x7F)
    {
        type |= 0x20;
    }
    if (c > 0x20 && c < 0x7F && !(type & 0x104))
    {
        type |= 0x10;
    }
    return type;
}

// A guest critical section: the owner and recursion count live in its RTL_CRITICAL_SECTION, so the guest sees the
// same fields Windows keeps
struct CriticalSection final : Waitable
{
    CriticalSection(Memory& memory, const std::uint32_t address)
    : memory(memory)
    , address(address)
    {
    }

    auto signaled(const ThreadId waiter) const -> bool override
    {
        const auto owner = memory.read<std::uint32_t>(address + 0x0C);
        return owner == 0 || owner == waiter;
    }

    void acquire(const ThreadId waiter) override
    {
        const auto owner = memory.read<std::uint32_t>(address + 0x0C);
        memory.write<std::uint32_t>(address + 0x08, owner == waiter ? memory.read<std::uint32_t>(address + 0x08) + 1 : 1);
        memory.write<std::uint32_t>(address + 0x0C, waiter);
    }

    Memory&       memory;
    std::uint32_t address;
};

} // namespace

void installKernel32(Process& p)
{
    auto&      boundary = p.machine.boundary();
    auto&      memory   = p.machine.memory();
    const auto add      = [&](const char* name, const std::uint8_t argc, Handler handler = {})
    {
        boundary.add(Function{ "kernel32.dll", name, Convention::Stdcall, argc, std::move(handler) });
    };
    const auto constant = [&](const char* name, const std::uint8_t argc, const std::uint32_t value)
    {
        add(name, argc, [value](Call&)
            {
                return ret(value);
            });
    };
    const auto threadId = [&p]()
    {
        return p.thread().id;
    };
    auto sections = std::make_shared<std::map<std::uint32_t, std::shared_ptr<CriticalSection>>>();
    auto regions  = std::make_shared<std::map<std::uint32_t, std::uint32_t>>(); // VirtualAlloc base -> size

    // Process, version and system
    constant("GetVersion", 0, 0x0A280105); // Windows XP, build 2600
    add("GetVersionExA", 1, [&memory](Call& call)
        {
            const auto info = call.arg(0);
            for (auto [offset, value] : { std::pair{ 4u, 5u }, { 8u, 1u }, { 12u, 2600u }, { 16u, 2u } })
            {
                memory.write<std::uint32_t>(info + offset, value);
            }
            memory.fill(info + 20, 0, 128);
            return ret(1);
        });
    constant("GetCurrentProcess", 0, kProcessPseudo);
    constant("GetCurrentThread", 0, kThreadPseudo);
    add("GetCurrentProcessId", 0, [&p, &memory](Call&)
        {
            return ret(memory.read<std::uint32_t>(p.thread().teb + 0x20));
        });
    add("GetCurrentThreadId", 0, [threadId](Call&)
        {
            return ret(threadId());
        });
    add("GetCommandLineA", 0, [&p](Call&)
        {
            return ret(p.commandLine);
        });
    add("GetEnvironmentStrings", 0, [&p](Call&)
        {
            return ret(p.environment);
        });
    add("GetEnvironmentStringsW", 0, [&p](Call&)
        {
            return ret(p.environment);
        });
    constant("FreeEnvironmentStringsA", 1, 1);
    constant("FreeEnvironmentStringsW", 1, 1);
    add("GetEnvironmentVariableA", 3, [&p](Call&)
        {
            p.setLastError(kErrorEnvvarNotFound);
            return ret(0);
        });
    constant("SetEnvironmentVariableA", 2, 1);
    add("GetStartupInfoA", 1, [&memory](Call& call)
        {
            memory.fill(call.arg(0), 0, 68);
            memory.write<std::uint32_t>(call.arg(0), 68);
            return ret();
        });
    constant("GetStdHandle", 1, kInvalidHandle);
    constant("SetStdHandle", 2, 1);
    add("SetHandleCount", 1, [](Call& call)
        {
            return ret(call.arg(0));
        });
    constant("GetACP", 0, kCodePage);
    constant("GetOEMCP", 0, kCodePage);
    constant("GetSystemDefaultLCID", 0, kLocale);
    constant("GetLocaleInfoA", 4, 0);
    add("IsValidCodePage", 1, [](Call& call)
        {
            return ret(call.arg(0) == kCodePage || call.arg(0) == 1252 || call.arg(0) == 437);
        });
    add("GetCPInfo", 2, [&memory](Call& call)
        {
            const auto info = call.arg(1);
            memory.fill(info, 0, 20);
            memory.write<std::uint32_t>(info, call.arg(0) == kCodePage ? 2 : 1);
            memory.write<std::uint8_t>(info + 4, '?');
            if (call.arg(0) == kCodePage)
            {
                for (auto [offset, value] : { std::pair{ 6u, 0x81 }, { 7u, 0x9F }, { 8u, 0xE0 }, { 9u, 0xFC } })
                {
                    memory.write<std::uint8_t>(info + offset, static_cast<std::uint8_t>(value)); // Shift-JIS lead bytes
                }
            }
            return ret(1);
        });
    constant("IsDebuggerPresent", 0, 0);
    constant("IsProcessorFeaturePresent", 1, 0); // no MMX or SSE, as CPUID says
    add("SetUnhandledExceptionFilter", 1, [&p](Call& call)
        {
            const auto previous = p.unhandledFilter;
            p.unhandledFilter   = call.arg(0);
            return ret(previous);
        });
    add("ExitProcess", 1, [](Call& call) -> Outcome
        {
            return Halt{ std::format("ExitProcess({})", call.arg(0)) };
        });
    add("TerminateProcess", 2, [](Call& call) -> Outcome
        {
            return Halt{ std::format("TerminateProcess({})", call.arg(1)) };
        });
    add("OutputDebugStringA", 1, [&p](Call& call)
        {
            p.trace.push_back(std::format("OutputDebugStringA {}", call.argString(0)));
            return ret();
        });
    add("GetLastError", 0, [&p, &memory](Call&)
        {
            return ret(memory.read<std::uint32_t>(p.thread().teb + 0x34));
        });
    add("SetLastError", 1, [&p](Call& call)
        {
            p.setLastError(call.arg(0));
            return ret();
        });
    constant("FormatMessageA", 7, 0);
    add("GetVolumeInformationA", 8, [&memory](Call& call)
        {
            if (call.arg(1) && call.arg(2))
            {
                copyString(memory, call.arg(1), call.arg(2), "TETRAMASTER");
            }
            for (auto [index, value] : { std::pair{ 3u, 0x1234ABCDu }, { 4u, 255u }, { 5u, 0x0002002Fu } })
            {
                if (call.arg(index))
                {
                    memory.write<std::uint32_t>(call.arg(index), value);
                }
            }
            if (call.arg(6) && call.arg(7))
            {
                copyString(memory, call.arg(6), call.arg(7), "NTFS");
            }
            return ret(1);
        });
    add("GetDiskFreeSpaceA", 5, [&memory](Call& call)
        {
            for (auto [index, value] : { std::pair{ 1u, 8u }, { 2u, 512u }, { 3u, 0x100000u }, { 4u, 0x200000u } })
            {
                if (call.arg(index))
                {
                    memory.write<std::uint32_t>(call.arg(index), value);
                }
            }
            return ret(1);
        });

    // Time
    add("GetTickCount", 0, [&p](Call&)
        {
            return ret(static_cast<std::uint32_t>(p.machine.scheduler().now() + 1000));
        });
    const auto now = []()
    {
        return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    };
    add("GetLocalTime", 1, [&memory, now](Call& call)
        {
            writeSystemTime(memory, call.arg(0), now());
            return ret();
        });
    add("GetSystemTime", 1, [&memory, now](Call& call)
        {
            writeSystemTime(memory, call.arg(0), now());
            return ret();
        });
    add("GetTimeZoneInformation", 1, [&memory](Call& call)
        {
            memory.fill(call.arg(0), 0, 172);
            return ret(0); // TIME_ZONE_ID_UNKNOWN
        });
    add("FileTimeToLocalFileTime", 2, [&memory](Call& call)
        {
            memory.write<std::uint64_t>(call.arg(1), memory.read<std::uint64_t>(call.arg(0)));
            return ret(1);
        });
    add("FileTimeToSystemTime", 2, [&memory](Call& call)
        {
            // 100 ns ticks since 1601-01-01
            const auto ticks = memory.read<std::uint64_t>(call.arg(0));
            const auto unix  = std::chrono::milliseconds{ static_cast<std::int64_t>(ticks / 10000) - 11644473600000LL };
            writeSystemTime(memory, call.arg(1), std::chrono::sys_time<std::chrono::milliseconds>{ unix });
            return ret(1);
        });
    add("Sleep", 1, [](Call& call) -> Outcome
        {
            if (call.arg(0) == 0)
            {
                return ret();
            }
            return Block{ Wait{ {}, false, call.arg(0) }, [](const WaitResult&)
                          {
                              return Return{};
                          } };
        });
    add("SetThreadExecutionState", 1, [](Call&)
        {
            return ret(0x80000000);
        }); // the previous state: ES_CONTINUOUS

    // Modules
    const auto moduleHandle = [&p](Call& call, const unsigned index) -> std::uint32_t
    {
        if (call.arg(index) == 0)
        {
            return p.game.base;
        }
        const auto name = moduleName(call.argString(index));
        if (name == lowerText(p.gameFile) || name == "tm.dll")
        {
            return p.game.base;
        }
        if (const auto base = p.machine.boundary().module(name))
        {
            return *base;
        }
        p.setLastError(kErrorModNotFound);
        return 0;
    };
    add("GetModuleHandleA", 1, [moduleHandle](Call& call)
        {
            return ret(moduleHandle(call, 0));
        });
    // ponytail: only the synthetic modules load; a DLL nobody stands in for fails as missing
    add("LoadLibraryA", 1, [moduleHandle](Call& call)
        {
            return ret(moduleHandle(call, 0));
        });
    constant("FreeLibrary", 1, 1);
    constant("DisableThreadLibraryCalls", 1, 1);
    add("GetProcAddress", 2, [&p, &memory](Call& call)
        {
            const auto module = call.arg(0);
            const auto name   = call.arg(1) < 0x10000 ? std::format("#{}", call.arg(1)) : call.argString(1);
            auto       found  = std::uint32_t{};
            if (module == p.game.base || p.modules.contains(module))
            {
                found = findExport(memory, module, name);
            }
            if (!found)
            {
                p.setLastError(kErrorProcNotFound);
            }
            return ret(found);
        });
    add("GetModuleFileNameA", 3, [&p, &memory](Call& call)
        {
            const auto module = call.arg(0);
            auto       path   = std::format("{}\\{}", p.installRoot, p.gameFile);
            if (const auto found = p.modules.find(module); found != p.modules.end())
            {
                path = std::format("C:\\WINDOWS\\system32\\{}", found->second);
            }
            return ret(copyString(memory, call.arg(1), call.arg(2), path));
        });
    add("GetShortPathNameA", 3, [&memory](Call& call)
        {
            const auto path = call.argString(0);
            if (call.arg(2) <= path.size())
            {
                return ret(static_cast<std::uint32_t>(path.size()) + 1);
            }
            memory.writeString(call.arg(1), path);
            return ret(static_cast<std::uint32_t>(path.size()));
        });

    // Memory
    add("VirtualAlloc", 4, [&memory, regions](Call& call)
        {
            const auto address = call.arg(0);
            const auto size    = call.arg(1);
            if (address)
            {
                const auto page = address & ~(kPageSize - 1);
                memory.map(page, ((address + size + kPageSize - 1) & ~(kPageSize - 1)) - page);
                return ret(call.arg(2) & kMemReserve ? address & ~0xFFFFu : page);
            }
            const auto rounded = (size + 0xFFFF) & ~0xFFFFu;
            const auto base    = memory.allocate(rounded, 0x10000);
            (*regions)[base]   = rounded;
            return ret(base);
        });
    add("VirtualFree", 3, [&memory, regions](Call& call)
        {
            const auto found = regions->find(call.arg(0));
            if (call.arg(2) & kMemRelease && found != regions->end())
            {
                memory.unmap(found->first, found->second);
                regions->erase(found);
            }
            return ret(1);
        });
    add("VirtualProtect", 4, [&memory](Call& call)
        {
            if (call.arg(3))
            {
                memory.write<std::uint32_t>(call.arg(3), kPageExecuteRW); // every page is read, write and execute
            }
            return ret(1);
        });
    const auto bad = [&memory](const std::uint32_t address, const std::uint32_t size) -> std::uint32_t
    {
        return size != 0 && !memory.mapped(address, size);
    };
    add("IsBadReadPtr", 2, [bad](Call& call)
        {
            return ret(bad(call.arg(0), call.arg(1)));
        });
    add("IsBadWritePtr", 2, [bad](Call& call)
        {
            return ret(bad(call.arg(0), call.arg(1)));
        });
    add("IsBadCodePtr", 1, [bad](Call& call)
        {
            return ret(bad(call.arg(0), 1));
        });
    add("HeapCreate", 3, [](Call&)
        {
            return ret(kHeapHandle);
        });
    constant("HeapDestroy", 1, 1);
    add("HeapAlloc", 3, [&p](Call& call)
        {
            return ret(p.allocate(call.arg(2)));
        });
    add("HeapFree", 3, [&p](Call& call)
        {
            p.free(call.arg(2));
            return ret(1);
        });
    add("HeapSize", 3, [&p](Call& call)
        {
            return ret(p.blockSize(call.arg(2)).value_or(0xFFFFFFFF));
        });
    add("HeapReAlloc", 4, [&p, &memory](Call& call)
        {
            const auto old  = call.arg(2);
            const auto size = call.arg(3);
            const auto was  = p.blockSize(old);
            if (!was)
            {
                return ret(0);
            }
            const auto block = p.allocate(size);
            auto       bytes = std::vector<std::uint8_t>(std::min(*was, size));
            memory.read(old, bytes);
            memory.write(block, std::span<const std::uint8_t>{ bytes });
            p.free(old);
            return ret(block);
        });
    add("LocalAlloc", 2, [&p](Call& call)
        {
            return ret(p.allocate(call.arg(1)));
        });
    add("LocalFree", 1, [&p](Call& call)
        {
            p.free(call.arg(0));
            return ret(0);
        });

    // Synchronisation and threads
    add("InterlockedIncrement", 1, [&memory](Call& call)
        {
            const auto value = memory.read<std::uint32_t>(call.arg(0)) + 1;
            memory.write<std::uint32_t>(call.arg(0), value);
            return ret(value);
        });
    add("InterlockedDecrement", 1, [&memory](Call& call)
        {
            const auto value = memory.read<std::uint32_t>(call.arg(0)) - 1;
            memory.write<std::uint32_t>(call.arg(0), value);
            return ret(value);
        });
    add("InterlockedExchange", 2, [&memory](Call& call)
        {
            const auto old = memory.read<std::uint32_t>(call.arg(0));
            memory.write<std::uint32_t>(call.arg(0), call.arg(1));
            return ret(old);
        });
    add("InitializeCriticalSection", 1, [&memory, sections](Call& call)
        {
            const auto address = call.arg(0);
            memory.fill(address, 0, 24);
            memory.write<std::uint32_t>(address + 4, 0xFFFFFFFF); // LockCount
            sections->erase(address);
            return ret();
        });
    add("DeleteCriticalSection", 1, [sections](Call& call)
        {
            sections->erase(call.arg(0));
            return ret();
        });
    add("EnterCriticalSection", 1, [&memory, sections, threadId](Call& call) -> Outcome
        {
            auto& section = (*sections)[call.arg(0)];
            if (!section)
            {
                section = std::make_shared<CriticalSection>(memory, call.arg(0));
            }
            if (section->signaled(threadId()))
            {
                section->acquire(threadId());
                return ret();
            }
            return Block{ Wait{ { section }, false, kInfinite }, [](const WaitResult&)
                          {
                              return Return{};
                          } };
        });
    add("LeaveCriticalSection", 1, [&memory, threadId](Call& call)
        {
            const auto address = call.arg(0);
            if (memory.read<std::uint32_t>(address + 0x0C) == threadId())
            {
                const auto count = memory.read<std::uint32_t>(address + 0x08) - 1;
                memory.write<std::uint32_t>(address + 0x08, count);
                if (count == 0)
                {
                    memory.write<std::uint32_t>(address + 0x0C, 0);
                }
            }
            return ret();
        });
    add("TlsAlloc", 0, [&p](Call&)
        {
            for (auto slot = 0u; slot < 32; ++slot)
            {
                if (!(p.tlsSlots & (1u << slot)))
                {
                    p.tlsSlots |= 1u << slot;
                    return ret(slot);
                }
            }
            return ret(0xFFFFFFFF);
        });
    add("TlsFree", 1, [&p](Call& call)
        {
            p.tlsSlots &= ~(1u << (call.arg(0) & 31));
            return ret(1);
        });
    add("TlsGetValue", 1, [&p, &memory](Call& call)
        {
            p.setLastError(kErrorSuccess);
            return ret(memory.read<std::uint32_t>(p.thread().teb + 0xE10 + 4 * (call.arg(0) & 63)));
        });
    add("TlsSetValue", 2, [&p, &memory](Call& call)
        {
            memory.write<std::uint32_t>(p.thread().teb + 0xE10 + 4 * (call.arg(0) & 63), call.arg(1));
            return ret(1);
        });
    add("CreateMutexA", 3, [&p, threadId](Call& call)
        {
            const auto name  = call.argString(2);
            auto       mutex = std::shared_ptr<Mutex>{};
            if (!name.empty() && p.namedMutexes.contains(name))
            {
                mutex = p.namedMutexes[name];
                p.setLastError(kErrorAlreadyExists);
            }
            else
            {
                mutex = std::make_shared<Mutex>();
                if (call.arg(1))
                {
                    mutex->acquire(threadId());
                }
                if (!name.empty())
                {
                    p.namedMutexes[name] = mutex;
                }
                p.setLastError(kErrorSuccess);
            }
            return ret(p.addHandle(mutex));
        });
    add("ReleaseMutex", 1, [&p, threadId](Call& call)
        {
            const auto mutex = p.handleAs<Mutex>(call.arg(0));
            if (!mutex || mutex->owner != threadId() || mutex->count == 0)
            {
                p.setLastError(kErrorNotOwner);
                return ret(0);
            }
            if (--mutex->count == 0)
            {
                mutex->owner = 0;
            }
            return ret(1);
        });
    add("WaitForSingleObject", 2, [&p, threadId](Call& call) -> Outcome
        {
            const auto object = p.handle(call.arg(0));
            if (!object)
            {
                p.setLastError(kErrorInvalidHandle);
                return ret(kWaitFailed);
            }
            if (object->signaled(threadId()))
            {
                object->acquire(threadId());
                return ret(kWaitObject0);
            }
            if (call.arg(1) == 0)
            {
                return ret(kWaitTimeout);
            }
            return Block{ Wait{ { object }, false, call.arg(1) },
                          [](const WaitResult& result)
                          {
                              return Return{ result.timedOut ? kWaitTimeout : kWaitObject0 };
                          } };
        });
    add("CreateThread", 6, [&p, &memory](Call& call)
        {
            auto& boundary = p.machine.boundary();
            auto& thread   = p.machine.scheduler().create(call.arg(2), call.arg(3), call.arg(1), (call.arg(4) & kCreateSuspended) != 0, boundary.resolve("tmweb", "thread.exit") + TrapRange::kTrapOffset);
            p.trace.push_back(std::format("CreateThread {:#x} {:#x}", call.arg(2), call.arg(3)));
            if (call.arg(5))
            {
                memory.write<std::uint32_t>(call.arg(5), thread.id);
            }
            return ret(p.addHandle(thread.handle));
        });
    // A thread's start routine returns here, as into BaseThreadStart: the thread exits with its EAX
    boundary.add(Function{ "tmweb", "thread.exit", Convention::Cdecl, 0, [&p](Call& call) -> Outcome
                           {
                               p.machine.scheduler().exit(p.thread(), call.context().r[kEax]);
                               return Resume{};
                           } });
    add("ExitThread", 1, [&p](Call& call) -> Outcome
        {
            p.machine.scheduler().exit(p.thread(), call.arg(0));
            return Resume{};
        });
    constant("SetThreadPriority", 2, 1);
    add("CloseHandle", 1, [&p](Call& call)
        {
            if (!p.closeHandle(call.arg(0)))
            {
                p.setLastError(kErrorInvalidHandle);
                return ret(0);
            }
            return ret(1);
        });

    // Exceptions
    add("RaiseException", 4, [&p, &memory](Call& call) -> Outcome
        {
            // Continuing returns from RaiseException, so the CONTEXT holds the caller's eip and the popped stack
            auto&      context     = call.context();
            const auto code        = call.arg(0);
            const auto count       = std::min(call.arg(2), 15u);
            const auto back        = call.returnAddress();
            auto       information = std::vector<std::uint32_t>(count);
            for (auto i = 0u; i < count; ++i)
            {
                information[i] = memory.read<std::uint32_t>(call.arg(3) + 4 * i);
            }
            context.r[kEsp] += 4 + 16;
            context.eip = back;
            if (!p.machine.raise(code, back, information))
            {
                return Halt{ std::format("unhandled exception {:#x} raised from {:#x}", code, back) };
            }
            return Resume{};
        });
    // ponytail: unlinks the frames up to the target without calling their unwind handlers (needs callGuest);
    // __finally blocks inside the unwound frames do not run
    add("RtlUnwind", 4, [&p, &memory](Call& call)
        {
            if (call.arg(0))
            {
                memory.write<std::uint32_t>(p.thread().teb, call.arg(0));
            }
            return ret(call.arg(3));
        });

    // Strings
    add("lstrlenA", 1, [](Call& call)
        {
            return ret(static_cast<std::uint32_t>(call.argString(0).size()));
        });
    add("lstrlenW", 1, [](Call& call)
        {
            return ret(static_cast<std::uint32_t>(call.argWideString(0).size()));
        });
    add("lstrcpyA", 2, [&memory](Call& call)
        {
            memory.writeString(call.arg(0), call.argString(1));
            return ret(call.arg(0));
        });
    add("lstrcatA", 2, [&memory](Call& call)
        {
            memory.writeString(call.arg(0), call.argString(0) + call.argString(1));
            return ret(call.arg(0));
        });
    add("MultiByteToWideChar", 6, [&p, &memory](Call& call)
        {
            const auto text = narrowArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3)));
            auto       wide = std::u16string(text.begin(), text.end());
            std::ranges::transform(text, wide.begin(), [](const char c)
                                   {
                                       return static_cast<char16_t>(static_cast<unsigned char>(c));
                                   });
            return ret(copyOut(p, wide, call.arg(4), call.arg(5)));
        });
    add("WideCharToMultiByte", 8, [&p, &memory](Call& call)
        {
            const auto wide = wideArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3)));
            auto       text = std::string(wide.size(), '?');
            std::ranges::transform(wide, text.begin(), [](const char16_t c)
                                   {
                                       return c < 0x100 ? static_cast<char>(c) : '?';
                                   });
            return ret(copyOut(p, text, call.arg(4), call.arg(5)));
        });
    add("LCMapStringA", 6, [&p, &memory](Call& call)
        {
            auto text = narrowArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3)));
            for (auto& c : text)
            {
                if (call.arg(1) & kLowercase)
                {
                    c = lowerAscii(c);
                }
                else if (call.arg(1) & kUppercase && c >= 'a' && c <= 'z')
                {
                    c = static_cast<char>(c - 32);
                }
            }
            return ret(copyOut(p, text, call.arg(4), call.arg(5)));
        });
    add("LCMapStringW", 6, [&p, &memory](Call& call)
        {
            auto text = wideArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3)));
            for (auto& c : text)
            {
                if (call.arg(1) & kLowercase && c >= u'A' && c <= u'Z')
                {
                    c = static_cast<char16_t>(c + 32);
                }
                else if (call.arg(1) & kUppercase && c >= u'a' && c <= u'z')
                {
                    c = static_cast<char16_t>(c - 32);
                }
            }
            return ret(copyOut(p, text, call.arg(4), call.arg(5)));
        });
    add("GetStringTypeA", 5, [&memory](Call& call)
        {
            const auto text = narrowArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3)));
            for (auto i = 0u; i < text.size(); ++i)
            {
                memory.write<std::uint16_t>(call.arg(4) + 2 * i, characterType(static_cast<unsigned char>(text[i])));
            }
            return ret(1);
        });
    add("GetStringTypeW", 4, [&memory](Call& call)
        {
            const auto text = wideArgument(memory, call.arg(1), static_cast<std::int32_t>(call.arg(2)));
            for (auto i = 0u; i < text.size(); ++i)
            {
                memory.write<std::uint16_t>(call.arg(3) + 2 * i, characterType(text[i]));
            }
            return ret(1);
        });
    const auto compare = [](auto a, auto b, const bool ignoreCase) -> std::uint32_t
    {
        while (!a.empty() && a.back() == 0)
        {
            a.pop_back();
        }
        while (!b.empty() && b.back() == 0)
        {
            b.pop_back();
        }
        if (ignoreCase)
        {
            for (auto* text : { &a, &b })
            {
                for (auto& c : *text)
                {
                    c = c >= 'A' && c <= 'Z' ? static_cast<std::remove_reference_t<decltype(c)>>(c + 32) : c;
                }
            }
        }
        const auto order = a <=> b;
        return order < 0 ? 1 : order > 0 ? 3
                                         : 2; // CSTR_LESS_THAN, CSTR_EQUAL, CSTR_GREATER_THAN
    };
    add("CompareStringA", 6, [&memory, compare](Call& call)
        {
            return ret(compare(narrowArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3))),
                               narrowArgument(memory, call.arg(4), static_cast<std::int32_t>(call.arg(5))),
                               call.arg(1) & kIgnoreCase));
        });
    add("CompareStringW", 6, [&memory, compare](Call& call)
        {
            return ret(compare(wideArgument(memory, call.arg(2), static_cast<std::int32_t>(call.arg(3))),
                               wideArgument(memory, call.arg(4), static_cast<std::int32_t>(call.arg(5))),
                               call.arg(1) & kIgnoreCase));
        });

    // Files
    add("CreateFileA", 7, [&p](Call& call)
        {
            const auto guest       = call.argString(0);
            const auto access      = call.arg(1);
            const auto disposition = call.arg(4);
            const auto path        = p.hostPath(guest);
            auto       data        = path ? p.files.read(*path) : std::nullopt;
            const auto writes      = (access & kGenericWrite) || disposition == kCreateNew || disposition == kCreateAlways ||
                                     (disposition == kOpenAlways && !data) || (disposition != kOpenExisting && disposition != kOpenAlways);
            p.trace.push_back(std::format("CreateFileA {} {}{}", guest, writes ? "write" : "read", data ? "" : " missing"));
            // ponytail: read-only; writes to pub/, logs/ and settings/ land when a screen needs them
            if (writes)
            {
                p.setLastError(kErrorAccessDenied);
                return ret(kInvalidHandle);
            }
            if (!data)
            {
                p.setLastError(path ? kErrorFileNotFound : kErrorPathNotFound);
                return ret(kInvalidHandle);
            }
            auto file  = std::make_shared<File>();
            file->path = *path;
            file->data = std::make_shared<const std::vector<std::uint8_t>>(std::move(*data));
            p.setLastError(kErrorSuccess);
            return ret(p.addHandle(file));
        });
    add("ReadFile", 5, [&p, &memory](Call& call)
        {
            const auto file = p.handleAs<File>(call.arg(0));
            if (!file)
            {
                p.setLastError(kErrorInvalidHandle);
                return ret(0);
            }
            const auto size  = static_cast<std::uint32_t>(file->data->size());
            const auto start = std::min(file->position, size);
            const auto count = std::min(call.arg(2), size - start);
            memory.write(call.arg(1), std::span{ file->data->data() + start, count });
            file->position = start + count;
            if (call.arg(3))
            {
                memory.write<std::uint32_t>(call.arg(3), count);
            }
            return ret(1);
        });
    add("WriteFile", 5, [&p](Call&)
        {
            p.setLastError(kErrorAccessDenied);
            return ret(0);
        });
    add("SetFilePointer", 4, [&p, &memory](Call& call)
        {
            const auto file = p.handleAs<File>(call.arg(0));
            if (!file)
            {
                p.setLastError(kErrorInvalidHandle);
                return ret(0xFFFFFFFF);
            }
            if (call.arg(2))
            {
                memory.write<std::uint32_t>(call.arg(2), 0);
            }
            const auto origin = std::array<std::int64_t, 3>{ 0, file->position, static_cast<std::int64_t>(file->data->size()) };
            const auto target = origin[std::min(call.arg(3), 2u)] + static_cast<std::int32_t>(call.arg(1));
            if (target < 0)
            {
                p.setLastError(kErrorInvalidParameter);
                return ret(0xFFFFFFFF);
            }
            file->position = static_cast<std::uint32_t>(target);
            return ret(file->position);
        });
    add("GetFileSize", 2, [&p, &memory](Call& call)
        {
            const auto file = p.handleAs<File>(call.arg(0));
            if (!file)
            {
                p.setLastError(kErrorInvalidHandle);
                return ret(0xFFFFFFFF);
            }
            if (call.arg(1))
            {
                memory.write<std::uint32_t>(call.arg(1), 0);
            }
            return ret(static_cast<std::uint32_t>(file->data->size()));
        });
    add("GetFileType", 1, [&p](Call& call)
        {
            if (p.handleAs<File>(call.arg(0)))
            {
                return ret(1); // FILE_TYPE_DISK
            }
            p.setLastError(kErrorInvalidHandle);
            return ret(0);
        });
    constant("FlushFileBuffers", 1, 1);
    for (const auto* name : { "DeleteFileA", "SetEndOfFile" })
    {
        add(name, 1, [&p](Call&)
            {
                p.setLastError(kErrorAccessDenied);
                return ret(0);
            });
    }
    add("MoveFileA", 2, [&p](Call&)
        {
            p.setLastError(kErrorAccessDenied);
            return ret(0);
        });
    const auto writeFind = [&memory](const std::uint32_t out, const FileEntry& entry)
    {
        memory.fill(out, 0, 320);
        memory.write<std::uint32_t>(out, entry.directory ? 0x10 : 0x80); // FILE_ATTRIBUTE_DIRECTORY or _NORMAL
        memory.write<std::uint32_t>(out + 28, static_cast<std::uint32_t>(entry.size >> 32));
        memory.write<std::uint32_t>(out + 32, static_cast<std::uint32_t>(entry.size));
        copyString(memory, out + 44, 260, entry.name);
    };
    add("FindFirstFileA", 2, [&p, writeFind](Call& call)
        {
            const auto pattern = call.argString(0);
            const auto slash   = pattern.find_last_of("\\/");
            const auto folder  = slash == std::string::npos ? std::string{} : pattern.substr(0, slash);
            const auto mask    = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
            const auto path    = p.hostPath(folder);
            const auto listing = path ? p.files.list(*path) : std::nullopt;
            auto       find    = std::make_shared<Find>();
            for (const auto& entry : listing.value_or(std::vector<FileEntry>{}))
            {
                if (matches(mask, entry.name))
                {
                    find->entries.push_back(entry);
                }
            }
            p.trace.push_back(std::format("FindFirstFileA {} {}", pattern, find->entries.size()));
            if (find->entries.empty())
            {
                p.setLastError(listing ? kErrorFileNotFound : kErrorPathNotFound);
                return ret(kInvalidHandle);
            }
            writeFind(call.arg(1), find->entries[0]);
            find->next = 1;
            return ret(p.addHandle(find));
        });
    add("FindNextFileA", 2, [&p, writeFind](Call& call)
        {
            const auto find = p.handleAs<Find>(call.arg(0));
            if (!find || find->next >= find->entries.size())
            {
                p.setLastError(find ? kErrorNoMoreFiles : kErrorInvalidHandle);
                return ret(0);
            }
            writeFind(call.arg(1), find->entries[find->next++]);
            return ret(1);
        });
    add("FindClose", 1, [&p](Call& call)
        {
            return ret(p.closeHandle(call.arg(0)));
        });
    add("CreateFileMappingA", 6, [&p](Call& call)
        {
            const auto file = p.handleAs<File>(call.arg(0));
            if (!file)
            {
                p.setLastError(kErrorInvalidHandle);
                return ret(0);
            }
            auto mapping  = std::make_shared<Mapping>();
            mapping->data = file->data;
            p.trace.push_back(std::format("CreateFileMappingA {}", file->path));
            return ret(p.addHandle(mapping));
        });
    // ponytail: a view is a private copy that stays mapped; nothing maps a file for writing
    add("MapViewOfFile", 5, [&p, &memory](Call& call)
        {
            const auto mapping = p.handleAs<Mapping>(call.arg(0));
            const auto offset  = call.arg(2);
            if (!mapping || offset > mapping->data->size())
            {
                p.setLastError(kErrorInvalidParameter);
                return ret(0);
            }
            const auto available = static_cast<std::uint32_t>(mapping->data->size()) - offset;
            const auto size      = call.arg(4) ? std::min(call.arg(4), available) : available;
            const auto view      = memory.allocate((size + kPageSize - 1) & ~(kPageSize - 1), 0x10000);
            memory.write(view, std::span{ mapping->data->data() + offset, size });
            return ret(view);
        });
    constant("UnmapViewOfFile", 1, 1);
}

} // namespace tmweb::hle
