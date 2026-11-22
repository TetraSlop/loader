// The stand-ins called the way guest code calls them: arguments on the main thread's stack, then the trap
#include "hle/image.h"
#include "hle/process.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>

using namespace tmweb;
using namespace tmweb::hle;

namespace
{

// A runtime folder on disk with a few files
class Folder final
{
public:
    Folder()
    : path_(std::filesystem::temp_directory_path() / std::format("tmweb-hle-{}", counter_++))
    {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "data");
        write("data/gW000.dat", "first");
        write("data/gW001.dat", "second file");
        write("data/Other.bin", "x");
    }

    ~Folder()
    {
        auto error = std::error_code{};
        std::filesystem::remove_all(path_, error);
    }

    Folder(const Folder&)                    = delete;
    Folder(Folder&&)                         = delete;
    auto operator=(const Folder&) -> Folder& = delete;
    auto operator=(Folder&&) -> Folder&      = delete;

    auto path() const -> const std::filesystem::path&
    {
        return path_;
    }

private:
    void write(const char* name, const std::string_view text) const
    {
        auto file = std::ofstream(path_ / name, std::ios::binary);
        file << text;
    }

    static inline std::atomic<int> counter_ = 0;
    std::filesystem::path          path_;
};

struct World
{
    World()
    : files(folder.path())
    , process(machine, files, "C:\\PlayOnline\\SquareEnix\\TetraMaster")
    {
        install(process);
        machine.boundary().publish();
        finishInstall(process);
        machine.scheduler().createMain(0x10000);
        scratch = machine.memory().allocate(0x10000);
    }

    // Call module!name with stdcall arguments; returns EAX
    auto call(const char* module, const char* name, std::initializer_list<std::uint32_t> args) -> std::uint32_t
    {
        auto& memory  = machine.memory();
        auto& context = machine.scheduler().current().context;
        auto  sp      = machine.scheduler().current().stackBase - 0x400;
        memory.write<std::uint32_t>(sp, 0x10001000);
        auto offset = 4u;
        for (const auto value : args)
        {
            memory.write<std::uint32_t>(sp + offset, value);
            offset += 4;
        }
        context.r[kEsp]    = sp;
        const auto trap    = machine.boundary().resolve(module, name) + TrapRange::kTrapOffset;
        const auto outcome = machine.boundary().dispatch(machine, context, trap);
        REQUIRE(std::holds_alternative<Return>(outcome));
        machine.complete(context, trap, std::get<Return>(outcome));
        CHECK(context.r[kEsp] == sp + offset);
        return context.r[kEax];
    }

    auto text(const std::string_view value) -> std::uint32_t
    {
        const auto address = scratch + used;
        machine.memory().writeString(address, value);
        used += static_cast<std::uint32_t>(value.size() + 16) & ~15u;
        return address;
    }

    auto buffer(const std::uint32_t size) -> std::uint32_t
    {
        const auto address = scratch + used;
        used += (size + 15) & ~15u;
        return address;
    }

    auto lastError() -> std::uint32_t
    {
        return call("kernel32.dll", "GetLastError", {});
    }

    Folder        folder;
    DiskFiles     files;
    Machine       machine;
    Process       process;
    std::uint32_t scratch = 0;
    std::uint32_t used    = 0;
};

} // namespace

TEST_CASE("Guest paths map into the runtime folder and nowhere else")
{
    auto  world = World{};
    auto& p     = world.process;
    CHECK(p.hostPath("data/gW000.dat") == "data/gW000.dat");
    CHECK(p.hostPath(".\\data\\..\\data\\x.bin") == "data/x.bin");
    CHECK(p.hostPath("c:\\playonline\\SquareEnix\\TetraMaster\\TM_native.dll") == "TM_native.dll");
    CHECK(p.hostPath("C:\\PlayOnline\\SquareEnix\\TetraMaster") == "");
    CHECK_FALSE(p.hostPath("C:\\PlayOnline\\SquareEnix\\TetraMasterX\\a"));
    CHECK_FALSE(p.hostPath("D:\\other"));
    CHECK_FALSE(p.hostPath("..\\outside"));
    CHECK_FALSE(p.hostPath("\\WINDOWS\\win.ini"));
}

TEST_CASE("Files open, read, seek and list through the host file API, read-only")
{
    auto  world  = World{};
    auto& memory = world.machine.memory();

    const auto handle = world.call("kernel32.dll", "CreateFileA", { world.text("DATA\\GW001.DAT"), 0x80000000, 1, 0, 3, 0x80, 0 });
    REQUIRE(handle != kInvalidHandle);
    CHECK(world.call("kernel32.dll", "GetFileSize", { handle, 0 }) == 11);
    CHECK(world.call("kernel32.dll", "SetFilePointer", { handle, 7, 0, 0 }) == 7);
    const auto out  = world.buffer(16);
    const auto read = world.buffer(4);
    CHECK(world.call("kernel32.dll", "ReadFile", { handle, out, 16, read, 0 }) == 1);
    CHECK(memory.read<std::uint32_t>(read) == 4);
    CHECK(memory.readString(out) == "file");
    CHECK(world.call("kernel32.dll", "CloseHandle", { handle }) == 1);
    CHECK(world.call("kernel32.dll", "CloseHandle", { handle }) == 0);

    CHECK(world.call("kernel32.dll", "CreateFileA", { world.text("data\\none.bin"), 0x80000000, 1, 0, 3, 0, 0 }) == kInvalidHandle);
    CHECK(world.lastError() == kErrorFileNotFound);
    CHECK(world.call("kernel32.dll", "CreateFileA", { world.text("data\\gW000.dat"), 0x40000000, 0, 0, 2, 0, 0 }) == kInvalidHandle);
    CHECK(world.lastError() == kErrorAccessDenied);

    const auto found = world.buffer(320);
    const auto find  = world.call("kernel32.dll", "FindFirstFileA", { world.text("data/gW*.dat"), found });
    REQUIRE(find != kInvalidHandle);
    CHECK(memory.readString(found + 44) == "gW000.dat");
    CHECK(memory.read<std::uint32_t>(found + 32) == 5);
    CHECK(world.call("kernel32.dll", "FindNextFileA", { find, found }) == 1);
    CHECK(memory.readString(found + 44) == "gW001.dat");
    CHECK(world.call("kernel32.dll", "FindNextFileA", { find, found }) == 0);
    CHECK(world.lastError() == kErrorNoMoreFiles);
    CHECK(world.call("kernel32.dll", "FindFirstFileA", { world.text("data/*.none"), found }) == kInvalidHandle);
}

TEST_CASE("The protector can map its own file and find exports by walking the images")
{
    auto  world  = World{};
    auto& memory = world.machine.memory();
    std::ofstream(world.folder.path() / "TM_native.dll", std::ios::binary) << "MZ-not-really";

    const auto path = world.buffer(260);
    const auto size = world.call("kernel32.dll", "GetModuleFileNameA", { 0, path, 260 });
    CHECK(memory.readString(path) == "C:\\PlayOnline\\SquareEnix\\TetraMaster\\TM_native.dll");
    CHECK(size == 50);
    const auto file    = world.call("kernel32.dll", "CreateFileA", { path, 0x80000000, 1, 0, 3, 0, 0 });
    const auto mapping = world.call("kernel32.dll", "CreateFileMappingA", { file, 0, 2, 0, 0, 0 });
    const auto view    = world.call("kernel32.dll", "MapViewOfFile", { mapping, 4, 0, 0, 0 });
    CHECK(memory.readString(view) == "MZ-not-really");

    const auto kernel32 = world.call("kernel32.dll", "GetModuleHandleA", { world.text("KERNEL32") });
    CHECK(kernel32 == *world.machine.boundary().module("kernel32.dll"));
    const auto proc = world.call("kernel32.dll", "GetProcAddress", { kernel32, world.text("VirtualAlloc") });
    CHECK(proc == findExport(memory, kernel32, "VirtualAlloc"));
    CHECK(proc == world.machine.boundary().resolve("kernel32.dll", "VirtualAlloc"));
    CHECK(world.call("kernel32.dll", "GetProcAddress", { kernel32, world.text("NoSuchFunction") }) == 0);
    CHECK(world.lastError() == kErrorProcNotFound);
    const auto dsound = world.call("kernel32.dll", "GetModuleHandleA", { world.text("DSOUND.dll") });
    CHECK(world.call("kernel32.dll", "GetProcAddress", { dsound, 11 }) == world.machine.boundary().resolve("dsound.dll", "#11"));
    CHECK(world.call("kernel32.dll", "GetModuleHandleA", { world.text("shell32.dll") }) == 0);
}

TEST_CASE("The registry serves tmloader's virtual values and keeps what the guest stores")
{
    auto       world  = World{};
    auto&      memory = world.machine.memory();
    const auto key    = world.buffer(4);
    CHECK(world.call("advapi32.dll", "RegCreateKeyExA", { 0x80000002, world.text("SOFTWARE\\PlayOnlineUS\\SquareEnix\\TetraMaster"), 0, 0, 0, 0xF003F, 0, key, 0 }) == 0);
    const auto type   = world.buffer(4);
    const auto data   = world.buffer(64);
    const auto length = world.buffer(4);

    memory.write<std::uint32_t>(length, 64);
    CHECK(world.call("advapi32.dll", "RegQueryValueExA", { memory.read<std::uint32_t>(key), world.text("SOUND"), 0, type, data, length }) == 0);
    CHECK(memory.read<std::uint32_t>(type) == 4);
    CHECK(memory.read<std::uint32_t>(data) == 1);
    memory.write<std::uint32_t>(length, 64);
    CHECK(world.call("advapi32.dll", "RegQueryValueExA", { memory.read<std::uint32_t>(key), world.text("0002"), 0, type, data, length }) == 0);
    CHECK(memory.readString(data) == "C:\\PlayOnline\\SquareEnix\\TetraMaster");
    memory.write<std::uint32_t>(length, 2);
    CHECK(world.call("advapi32.dll", "RegQueryValueExA", { memory.read<std::uint32_t>(key), world.text("Path"), 0, type, data, length }) == kErrorMoreData);
    CHECK(memory.read<std::uint32_t>(length) == 37);
    CHECK(world.call("advapi32.dll", "RegQueryValueExA", { memory.read<std::uint32_t>(key), world.text("B_00OK"), 0, type, data, length }) == kErrorFileNotFound);

    // Keys nobody created do not open; created ones keep their values
    CHECK(world.call("advapi32.dll", "RegOpenKeyExA", { 0x80000001, world.text("SOFTWARE\\"), 0, 0x20019, key }) == kErrorFileNotFound);
    CHECK(world.call("advapi32.dll", "RegCreateKeyExA", { 0x80000000, world.text(".key"), 0, 0, 0, 0xF003F, 0, key, 0 }) == 0);
    memory.write<std::uint32_t>(data, 0xABCD);
    CHECK(world.call("advapi32.dll", "RegSetValueExA", { memory.read<std::uint32_t>(key), world.text(""), 0, 3, data, 4 }) == 0);
    CHECK(world.call("advapi32.dll", "RegOpenKeyExA", { 0x80000000, world.text(".KEY"), 0, 0x20019, key }) == 0);
}

TEST_CASE("Heap, TLS, last error, strings and the window the boot creates")
{
    auto  world  = World{};
    auto& memory = world.machine.memory();

    const auto block = world.call("kernel32.dll", "HeapAlloc", { 0x150000, 8, 40 });
    CHECK(world.call("kernel32.dll", "HeapSize", { 0x150000, 0, block }) == 40);
    world.call("kernel32.dll", "HeapFree", { 0x150000, 0, block });
    CHECK(world.call("kernel32.dll", "HeapAlloc", { 0x150000, 8, 33 }) == block);

    const auto slot = world.call("kernel32.dll", "TlsAlloc", {});
    world.call("kernel32.dll", "TlsSetValue", { slot, 0x1234 });
    world.call("kernel32.dll", "SetLastError", { 5 });
    CHECK(world.call("kernel32.dll", "TlsGetValue", { slot }) == 0x1234);
    CHECK(world.lastError() == 0);

    // -1 lengths count the terminator; a capacity of 0 asks for the length
    const auto wide = world.buffer(32);
    CHECK(world.call("kernel32.dll", "MultiByteToWideChar", { 932, 0, world.text("abc"), 0xFFFFFFFF, 0, 0 }) == 4);
    CHECK(world.call("kernel32.dll", "MultiByteToWideChar", { 932, 0, world.text("abc"), 0xFFFFFFFF, wide, 16 }) == 4);
    CHECK(memory.readWideString(wide) == u"abc");
    CHECK(world.call("kernel32.dll", "CompareStringA", { 0x411, 1, world.text("ABC"), 0xFFFFFFFF, world.text("abc"), 0xFFFFFFFF }) == 2);
    CHECK(world.call("kernel32.dll", "CompareStringA", { 0x411, 0, world.text("abc"), 3, world.text("abd"), 3 }) == 1);

    const auto mutex = world.call("kernel32.dll", "CreateMutexA", { 0, 0, world.text("TetraMaster") });
    CHECK(world.call("kernel32.dll", "WaitForSingleObject", { mutex, 0xFFFFFFFF }) == 0);
    CHECK(world.call("kernel32.dll", "ReleaseMutex", { mutex }) == 1);
    CHECK(world.call("kernel32.dll", "ReleaseMutex", { mutex }) == 0);
    world.call("kernel32.dll", "CreateMutexA", { 0, 0, world.text("TetraMaster") });
    CHECK(world.lastError() == kErrorAlreadyExists);

    // A mutex its owner never released is free once the owner exits
    const auto held  = world.process.handle(world.call("kernel32.dll", "CreateMutexA", { 0, 0, 0 }));
    auto&      other = world.machine.scheduler().create(0x10002000, 0, 0x10000, true, 0);
    const auto self  = world.machine.scheduler().current().id;
    held->acquire(other.id);
    CHECK_FALSE(held->signaled(self));
    world.machine.scheduler().exit(other, 0);
    CHECK(held->signaled(self));

    // A new thread starts suspended or ready, returning into ExitThread; its handle signals once it exits
    const auto id     = world.buffer(4);
    const auto thread = world.call("kernel32.dll", "CreateThread", { 0, 0, 0x10002000, 0x55, 4, id });
    auto*      added  = world.machine.scheduler().find(memory.read<std::uint32_t>(id));
    REQUIRE(added);
    CHECK(added->state == ThreadState::Suspended);
    CHECK(added->context.eip == 0x10002000);
    CHECK(memory.read<std::uint32_t>(added->context.r[kEsp]) == world.machine.boundary().resolve("tmweb", "thread.exit") + TrapRange::kTrapOffset);
    CHECK(memory.read<std::uint32_t>(added->context.r[kEsp] + 4) == 0x55);
    CHECK(added->teb != world.machine.scheduler().current().teb);
    CHECK(world.call("kernel32.dll", "WaitForSingleObject", { thread, 0 }) == 0x102);
    world.machine.scheduler().exit(*added, 0);
    CHECK(world.call("kernel32.dll", "WaitForSingleObject", { thread, 0 }) == 0);

    const auto info = world.buffer(48);
    memory.write<std::uint32_t>(info + 8, 0x101863D0);
    memory.write<std::uint32_t>(info + 40, world.text("Tetra Master"));
    CHECK(world.call("user32.dll", "RegisterClassExA", { info }) != 0);
    const auto window = world.call("user32.dll", "CreateWindowExA", { 0, world.text("Tetra Master"), world.text("Tetra Master"), 0x80000000, 0, 0, 640, 480, 0, 0, 0x10000000, 0 });
    CHECK(window != 0);
    REQUIRE(world.process.classes.size() == 1);
    CHECK(world.process.classes[0].procedure == 0x101863D0);
    CHECK(world.process.windows[0].width == 640);
    CHECK(world.call("user32.dll", "ShowCursor", { 0 }) == 0xFFFFFFFF);
}
