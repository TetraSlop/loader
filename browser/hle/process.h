#pragma once

// The Win32 side of one emulated process: guest handles, the heap, the registry, windows and the runtime folder,
// shared by the kernel32, user32, advapi32 and smaller stand-ins. install() registers them all with the boundary.

#include "core/machine.h"
#include "hle/files.h"
#include "hle/image.h"

#include <array>
#include <cstdint>
#include <deque>
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
    kErrorSuccess              = 0,
    kErrorFileNotFound         = 2,
    kErrorPathNotFound         = 3,
    kErrorAccessDenied         = 5,
    kErrorInvalidHandle        = 6,
    kErrorNoMoreFiles          = 18,
    kErrorFileExists           = 80,
    kErrorInvalidParameter     = 87,
    kErrorInsufficient         = 122,
    kErrorModNotFound          = 126,
    kErrorProcNotFound         = 127,
    kErrorAlreadyExists        = 183,
    kErrorMoreData             = 234,
    kErrorEnvvarNotFound       = 203,
    kErrorNotOwner             = 288,
    kErrorNoUnicodeTranslation = 1113,
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

using Bytes = std::shared_ptr<std::vector<std::uint8_t>>;

// An open file holds all of its bytes; writes change them, and a flush or the last close writes them back
struct File final : Object
{
    std::string   path; // relative to the runtime folder
    Bytes         data;
    std::uint32_t position = 0;
    bool          writable = false;
    bool          dirty    = false;
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
    std::uint32_t atom      = 0;
};

struct Window
{
    std::uint32_t handle    = 0;
    std::uint32_t procedure = 0;
    std::string   className;
    std::string   title;
    std::uint32_t style   = 0;
    std::int32_t  x       = 0; // the client area on the screen; windows have no frame
    std::int32_t  y       = 0;
    std::int32_t  width   = 0;
    std::int32_t  height  = 0;
    ThreadId      thread  = 0;
    bool          visible = false;
    bool          invalid = false; // needs a WM_PAINT
};

// A posted message, as MSG holds it
struct Message
{
    std::uint32_t window  = 0;
    std::uint32_t message = 0;
    std::uint32_t wParam  = 0;
    std::uint32_t lParam  = 0;
    std::uint32_t time    = 0;
    std::int32_t  x       = 0;
    std::int32_t  y       = 0;
};

// Keyboard and mouse events from the host (host::InputEvent's kinds and fields). Key codes are DirectInput's.
enum class InputKind : std::uint8_t
{
    Key,    // a: DIK code, b: 1 down, 0 up
    Move,   // a, b: the pointer in client pixels
    Button, // a: 0 left, 1 right, 2 middle; b: 1 down, 0 up
    Wheel,  // a: 120ths of a notch, away from the user positive
};

struct InputEvent
{
    InputKind    kind     = InputKind::Key;
    std::int32_t a        = 0;
    std::int32_t b        = 0;
    bool         repeat   = false; // a key down while it was already down
    bool         injected = false; // keybd_event
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
    // Whether the guest may write there: pub/, logs/ and settings/, as tmloader keeps writes inside the runtime
    static auto writable(std::string_view path) -> bool;
    // Write a changed file back; false when the host could not
    auto flush(File& file) -> bool;

    // Synthetic module names by image base, for GetProcAddress and GetModuleFileNameA
    std::map<std::uint32_t, std::string> modules;

    std::map<std::string, std::map<std::string, RegistryValue>> registry; // key path -> value name (lower case)

    std::vector<WindowClass> classes;
    std::vector<Window>      windows;
    std::vector<Hook>        hooks;

    // A host keyboard or mouse event, between run slices: the async state changes at once, and the event waits for
    // the window's thread to pump messages, which runs the low-level keyboard hooks and posts it
    void input(InputKind kind, std::int32_t a, std::int32_t b, bool injected = false);
    auto window(std::uint32_t handle) -> Window*;

    // ponytail: one message queue for the process, as TM has one window thread; per-thread queues if polcore's
    // threads make windows
    std::deque<InputEvent>    pending; // input not yet through the hooks
    std::deque<Message>       messages;
    bool                      quit     = false;
    std::uint32_t             quitCode = 0;
    std::array<bool, 256>     keys{}; // by DIK code
    std::array<bool, 3>       buttons{};
    std::int32_t              cursorX   = 320;
    std::int32_t              cursorY   = 240;
    std::uint32_t             active    = 0; // the active window, which has the focus
    bool                      appActive = false;
    std::shared_ptr<Waitable> queueReady; // signaled when GetMessageA has something to return
    std::int32_t              cursorCount     = 0;
    std::uint32_t             unhandledFilter = 0;
    std::uint32_t             tlsSlots        = 0; // TlsAlloc bitmap, 32 slots
    std::uint32_t             commandLine     = 0;
    std::uint32_t             environment     = 0;

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
