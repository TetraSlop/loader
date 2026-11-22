#pragma once

// The Win32 side of one emulated process: guest handles, the heap, the registry, windows and the runtime folder,
// shared by the kernel32, user32, advapi32 and smaller stand-ins. install() registers them all with the boundary.

#include "core/machine.h"
#include "hle/files.h"
#include "hle/image.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tmweb::hle
{

// Win32 error codes the stand-ins set
enum Error : std::uint32_t
{
    kErrorSuccess          = 0,
    kErrorFileNotFound     = 2,
    kErrorPathNotFound     = 3,
    kErrorAccessDenied     = 5,
    kErrorInvalidHandle    = 6,
    kErrorNoMoreFiles      = 18,
    kErrorInvalidParameter = 87,
    kErrorInsufficient     = 122,
    kErrorModNotFound      = 126,
    kErrorProcNotFound     = 127,
    kErrorAlreadyExists    = 183,
    kErrorMoreData         = 234,
    kErrorEnvvarNotFound   = 203,
    kErrorNotOwner         = 288,
};

constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFF;

// Everything a guest handle names can be waited on; files, finds and keys are always signaled
struct Object : Waitable
{
    auto signaled(ThreadId) const -> bool override
    {
        return true;
    }
};

using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;

struct File final : Object
{
    std::string   path; // relative to the runtime folder
    Bytes         data;
    std::uint32_t position = 0;
};

struct Find final : Object
{
    std::vector<FileEntry> entries;
    std::size_t            next = 0;
};

struct Mapping final : Object
{
    Bytes data;
};

struct Key final : Object
{
    std::string path; // lower case, "hklm\software\..."
};

struct Mutex final : Waitable
{
    ThreadId      owner = 0;
    std::uint32_t count = 0;

    auto signaled(ThreadId waiter) const -> bool override;
    void acquire(ThreadId waiter) override;
};

struct WindowClass
{
    std::string   name;
    std::uint32_t procedure = 0;
};

struct Window
{
    std::uint32_t handle = 0;
    std::string   className;
    std::string   title;
    std::uint32_t style  = 0;
    std::int32_t  width  = 0;
    std::int32_t  height = 0;
};

struct Hook
{
    std::uint32_t handle    = 0;
    std::uint32_t type      = 0;
    std::uint32_t procedure = 0;
};

struct RegistryValue
{
    std::uint32_t             type = 0;
    std::vector<std::uint8_t> data;
};

struct Process
{
    Process(Machine& machine, HostFiles& files, std::string installRoot);

    Machine&    machine;
    HostFiles&  files;
    std::string installRoot; // the runtime folder as the guest sees it: C:\PlayOnline\SquareEnix\TetraMaster
    Image       game;        // set once TM.dll is mapped
    std::string gameFile = "TM_native.dll";

    // Guest handles: multiples of 4 from 0x100, as Windows hands them out
    auto addHandle(std::shared_ptr<Waitable> object) -> std::uint32_t;
    auto handle(std::uint32_t value) const -> std::shared_ptr<Waitable>;

    template <class T>
    auto handleAs(const std::uint32_t value) const -> std::shared_ptr<T>
    {
        return std::dynamic_pointer_cast<T>(handle(value));
    }

    auto closeHandle(std::uint32_t value) -> bool;

    // One heap for HeapAlloc, LocalAlloc and the CRT; blocks are zeroed
    auto allocate(std::uint32_t size) -> std::uint32_t;
    void free(std::uint32_t address);
    auto blockSize(std::uint32_t address) const -> std::optional<std::uint32_t>;

    // The current thread's TEB fields
    auto thread() -> Thread&;
    void setLastError(std::uint32_t error);

    // A guest path as a path in the runtime folder, or nothing outside it. Relative paths start at the folder,
    // which is the process's current directory.
    auto hostPath(std::string_view guestPath) const -> std::optional<std::string>;

    // Synthetic module names by image base, for GetProcAddress and GetModuleFileNameA
    std::map<std::uint32_t, std::string> modules;

    std::map<std::string, std::map<std::string, RegistryValue>> registry; // key path -> value name (lower case)

    std::vector<WindowClass> classes;
    std::vector<Window>      windows;
    std::vector<Hook>        hooks;
    std::int32_t             cursorCount     = 0;
    std::uint32_t            unhandledFilter = 0;
    std::uint32_t            tlsSlots        = 0; // TlsAlloc bitmap, 32 slots
    std::uint32_t            commandLine     = 0;
    std::uint32_t            environment     = 0;

    std::map<std::string, std::shared_ptr<Mutex>> namedMutexes;

    // What the guest did, one line each, for tests and the log: "CreateFileA data/gW000.dat read"
    std::vector<std::string> trace;

private:
    std::map<std::uint32_t, std::shared_ptr<Waitable>>  handles_;
    std::uint32_t                                       nextHandle_ = 0x100;
    std::map<std::uint32_t, std::uint32_t>              blocks_;    // live block -> size
    std::map<std::uint32_t, std::vector<std::uint32_t>> freeLists_; // rounded size -> freed blocks
};

// Register every stand-in with the machine's boundary. Call before Boundary::publish(), then fill in modules with
// finishInstall().
void install(Process& process);
void finishInstall(Process& process);

void installKernel32(Process& process);
void installUser32(Process& process);
void installAdvapi32(Process& process);
void installSmallModules(Process& process); // imm32, ole32, oleaut32, gdi32, and media placeholders

} // namespace tmweb::hle
