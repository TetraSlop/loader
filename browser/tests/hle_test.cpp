// The stand-ins called the way guest code calls them: arguments on the main thread's stack, then the trap
#include "hle/image.h"
#include "hle/process.h"
#include "hle/text.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

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
        machine.cpu().setTraps(machine.boundary().traps()); // callGuest runs guest code outside Machine::run
        scratch = machine.memory().allocate(0x10000);
    }

    // Guest code from its bytes; pieces above 0xFF are 32-bit little-endian operands
    auto code(std::initializer_list<std::uint32_t> pieces) -> std::uint32_t
    {
        auto bytes = std::vector<std::uint8_t>{};
        for (const auto piece : pieces)
        {
            if (piece > 0xFF)
            {
                for (auto shift = 0u; shift < 32; shift += 8)
                {
                    bytes.push_back(static_cast<std::uint8_t>(piece >> shift));
                }
            }
            else
            {
                bytes.push_back(static_cast<std::uint8_t>(piece));
            }
        }
        const auto address = buffer(static_cast<std::uint32_t>(bytes.size()));
        machine.memory().write(address, std::span<const std::uint8_t>{ bytes });
        return address;
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

TEST_CASE("Shift-JIS converts on Microsoft's table, both ways")
{
    using namespace tmweb::hle::text;
    auto       invalid = false;
    const auto title   = std::string{ "\x83\x65\x83\x67\x83\x89\x83\x7D\x83\x58\x83\x5E\x81\x5B" };
    const auto wide    = std::u16string{ u"テトラマスター" }; // テトラマスター
    CHECK(decode(kShiftJis, title, invalid) == wide);
    CHECK_FALSE(invalid);
    CHECK(decode(kShiftJis, "A\xB1", invalid) == u"Aｱ");     // halfwidth katakana
    CHECK(decode(kShiftJis, "\x81\x20", invalid) == u"・ "); // a lead byte without a trail
    CHECK(invalid);

    auto used = false;
    CHECK(encode(kShiftJis, wide, '?', used) == title);
    CHECK_FALSE(used);
    CHECK(encode(kShiftJis, u"∵", '?', used) == "\x81\xE6"); // JIS X 0208 over NEC row 13 (0x879A)
    CHECK(encode(kShiftJis, u"ⅰ", '?', used) == "\xFA\x40"); // IBM over NEC-selected IBM (0xEEEF)
    CHECK_FALSE(used);
    CHECK(encode(kShiftJis, u"é", '?', used) == "?");
    CHECK(used);
    CHECK(decode(kUtf8, "\xE3\x83\x86", invalid) == u"テ");
    CHECK(codePage(0) == kShiftJis);

    auto       world  = World{};
    auto&      memory = world.machine.memory();
    const auto out    = world.buffer(64);
    CHECK(world.call("kernel32.dll", "MultiByteToWideChar", { 0, 0, world.text(title), 0xFFFFFFFF, out, 32 }) == 8);
    CHECK(memory.readWideString(out) == wide);
    const auto narrow = world.buffer(64);
    CHECK(world.call("kernel32.dll", "WideCharToMultiByte", { 932, 0, out, 0xFFFFFFFF, narrow, 64, 0, 0 }) == 15);
    CHECK(memory.readString(narrow) == title);
}

TEST_CASE("Writes in pub/, logs/ and settings/ reach the host when flushed or closed")
{
    auto  world         = World{};
    auto& memory        = world.machine.memory();
    auto  written       = std::vector<std::string>{};
    world.files.written = [&written](const std::string_view path)
    {
        written.emplace_back(path);
    };

    const auto file = world.call("kernel32.dll", "CreateFileA", { world.text("pub\\home00\\option.bin"), 0xC0000000, 0, 0, 2, 0x80, 0 });
    REQUIRE(file != kInvalidHandle);
    CHECK(written == std::vector<std::string>{ "pub/home00/option.bin" }); // created empty at once
    const auto count = world.buffer(4);
    CHECK(world.call("kernel32.dll", "WriteFile", { file, world.text("hello"), 5, count, 0 }) == 1);
    CHECK(memory.read<std::uint32_t>(count) == 5);
    CHECK(world.call("kernel32.dll", "SetFilePointer", { file, 1, 0, 0 }) == 1);
    CHECK(world.call("kernel32.dll", "WriteFile", { file, world.text("EL"), 2, count, 0 }) == 1);
    CHECK(world.call("kernel32.dll", "FlushFileBuffers", { file }) == 1);
    CHECK(written.size() == 2);
    CHECK(world.call("kernel32.dll", "CloseHandle", { file }) == 1);
    CHECK(written.size() == 2); // nothing changed since the flush
    auto stored = std::ifstream(world.folder.path() / "pub" / "home00" / "option.bin", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(stored), {}) == "hELlo");

    // OPEN_ALWAYS keeps what is there and says so
    const auto again = world.call("kernel32.dll", "CreateFileA", { world.text("PUB/home00/OPTION.BIN"), 0xC0000000, 0, 0, 4, 0x80, 0 });
    CHECK(world.lastError() == kErrorAlreadyExists);
    CHECK(world.call("kernel32.dll", "GetFileSize", { again, 0 }) == 5);
    CHECK(world.call("kernel32.dll", "CloseHandle", { again }) == 1);

    // Elsewhere in the runtime folder writes are refused, as tmloader refuses them
    CHECK(world.call("kernel32.dll", "CreateFileA", { world.text("data\\gW000.dat"), 0x40000000, 0, 0, 3, 0, 0 }) == kInvalidHandle);
    CHECK(world.lastError() == kErrorAccessDenied);
    CHECK(world.call("kernel32.dll", "CreateFileA", { world.text("logs\\new.txt"), 0x40000000, 0, 0, 1, 0, 0 }) != kInvalidHandle);
    CHECK(world.call("kernel32.dll", "CreateFileA", { world.text("logs\\new.txt"), 0x40000000, 0, 0, 1, 0, 0 }) == kInvalidHandle);
    CHECK(world.lastError() == kErrorFileExists);
}

TEST_CASE("Messages reach the window procedure, and host keys go through the low-level hook")
{
    auto  world  = World{};
    auto& memory = world.machine.memory();
    auto& p      = world.process;
    // The window procedure logs each message and returns 1: mov eax,[esp+8]; mov ecx,[count];
    // mov [log+ecx*4],eax; inc ecx; mov [count],ecx; mov eax,1; ret 16
    const auto count     = world.buffer(4);
    const auto log       = world.buffer(4 * 256);
    const auto procedure = world.code({ 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x0D, count, 0x89, 0x04, 0x8D, log, 0x41, 0x89, 0x0D, count, 0xB8, 1, 0, 0, 0, 0xC2, 0x10, 0x00 });
    // The hook records the virtual key and swallows Q: mov eax,[esp+12]; mov eax,[eax]; mov [seen],eax;
    // cmp eax,'Q'; sete al; movzx eax,al; ret 12
    const auto seen     = world.buffer(4);
    const auto hook     = world.code({ 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x00, 0xA3, seen, 0x3D, 'Q', 0, 0, 0, 0x0F, 0x94, 0xC0, 0x0F, 0xB6, 0xC0, 0xC2, 0x0C, 0x00 });
    const auto messages = [&]()
    {
        auto out = std::vector<std::uint32_t>{};
        for (auto i = 0u; i < memory.read<std::uint32_t>(count); ++i)
        {
            out.push_back(memory.read<std::uint32_t>(log + 4 * i));
        }
        memory.write<std::uint32_t>(count, 0);
        return out;
    };

    const auto info = world.buffer(48);
    memory.write<std::uint32_t>(info + 8, procedure);
    memory.write<std::uint32_t>(info + 40, world.text("Tetra Master"));
    CHECK(world.call("user32.dll", "RegisterClassExA", { info }) != 0);
    world.call("user32.dll", "SetWindowsHookExA", { 13, hook, 0x10000000, 0 });
    const auto window = world.call("user32.dll", "CreateWindowExA", { 0, world.text("Tetra Master"), world.text("Tetra Master"), 0x90000000, 0, 0, 640, 480, 0, 0, 0x10000000, 0 });
    REQUIRE(window != 0);
    // Creation, then WS_VISIBLE shows and activates it
    CHECK(messages() == std::vector<std::uint32_t>{ 0x24, 0x81, 0x83, 0x01, 0x05, 0x03, 0x18, 0x46, 0x1C, 0x86, 0x06, 0x07, 0x47 });
    CHECK(world.call("user32.dll", "GetForegroundWindow", {}) == window);

    // The shown window needs painting until DefWindowProc validates it
    const auto msg = world.buffer(28);
    CHECK(world.call("user32.dll", "PeekMessageA", { msg, 0, 0, 0, 1 }) == 1);
    CHECK(memory.read<std::uint32_t>(msg + 4) == 0x0F);
    world.call("user32.dll", "DefWindowProcA", { window, 0x0F, 0, 0 });
    CHECK(world.call("user32.dll", "PeekMessageA", { msg, 0, 0, 0, 1 }) == 0);

    // Q goes to the hook, which swallows it; A reaches the queue, and TranslateMessage adds its character
    p.input(InputKind::Key, 0x10, 1);
    p.input(InputKind::Key, 0x1E, 1);
    CHECK(world.call("user32.dll", "GetAsyncKeyState", { 'A' }) == 0x8000);
    CHECK(world.call("user32.dll", "PeekMessageA", { msg, 0, 0, 0, 1 }) == 1);
    CHECK(memory.read<std::uint32_t>(seen) == 'A');
    CHECK(memory.read<std::uint32_t>(msg + 4) == 0x100);
    CHECK(memory.read<std::uint32_t>(msg + 8) == 'A');
    CHECK(memory.read<std::uint32_t>(msg + 12) == (1u | (0x1Eu << 16)));
    CHECK(world.call("user32.dll", "TranslateMessage", { msg }) == 1);
    CHECK(world.call("user32.dll", "DispatchMessageA", { msg }) == 1);
    CHECK(world.call("user32.dll", "PeekMessageA", { msg, 0, 0, 0, 1 }) == 1);
    CHECK(memory.read<std::uint32_t>(msg + 4) == 0x102);
    CHECK(memory.read<std::uint32_t>(msg + 8) == 'a');
    CHECK(messages() == std::vector<std::uint32_t>{ 0x100 });

    // The mouse, in client pixels; moves coalesce
    p.input(InputKind::Move, 10, 20);
    p.input(InputKind::Move, 30, 40);
    p.input(InputKind::Button, 0, 1);
    CHECK(world.call("user32.dll", "PeekMessageA", { msg, window, 0, 0, 1 }) == 1);
    CHECK(memory.read<std::uint32_t>(msg + 4) == 0x200);
    CHECK(memory.read<std::uint32_t>(msg + 12) == ((40u << 16) | 30u));
    CHECK(world.call("user32.dll", "PeekMessageA", { msg, window, 0, 0, 1 }) == 1);
    CHECK(memory.read<std::uint32_t>(msg + 4) == 0x201);
    const auto point = world.buffer(8);
    world.call("user32.dll", "GetCursorPos", { point });
    CHECK(memory.read<std::int32_t>(point) == 30);

    world.call("user32.dll", "PostQuitMessage", { 3 });
    CHECK(world.call("user32.dll", "GetMessageA", { msg, 0, 0, 0 }) == 0);
    CHECK(memory.read<std::uint32_t>(msg + 8) == 3);
}

TEST_CASE("RtlUnwind runs the unwind handlers of the frames it removes")
{
    auto  world  = World{};
    auto& memory = world.machine.memory();
    auto& thread = world.machine.scheduler().current();
    // Each handler logs the record's flags and its frame: mov eax,[esp+4]; mov eax,[eax+4]; mov ecx,[count];
    // mov [log+ecx*8],eax; mov eax,[esp+8]; mov [log+ecx*8+4],eax; inc ecx; mov [count],ecx; mov eax,1; ret
    const auto count   = world.buffer(4);
    const auto log     = world.buffer(64);
    const auto handler = world.code({ 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0x8B, 0x0D, count, 0x89, 0x04, 0xCD, log, 0x8B, 0x44, 0x24, 0x08, 0x89, 0x04, 0xCD, log + 4, 0x41, 0x89, 0x0D, count, 0xB8, 1, 0, 0, 0, 0xC3 });
    const auto outer   = thread.stackBase - 0x100;
    const auto middle  = thread.stackBase - 0x140;
    const auto inner   = thread.stackBase - 0x180;
    for (const auto& [frame, next] : { std::pair{ outer, 0xFFFFFFFFu }, { middle, outer }, { inner, middle } })
    {
        memory.write<std::uint32_t>(frame, next);
        memory.write<std::uint32_t>(frame + 4, handler);
    }
    memory.write<std::uint32_t>(thread.teb, inner);

    CHECK(world.call("kernel32.dll", "RtlUnwind", { outer, 0, 0, 0x77 }) == 0x77);
    REQUIRE(memory.read<std::uint32_t>(count) == 2);
    CHECK(memory.read<std::uint32_t>(log) == 2); // EH_UNWINDING
    CHECK(memory.read<std::uint32_t>(log + 4) == inner);
    CHECK(memory.read<std::uint32_t>(log + 12) == middle);
    CHECK(memory.read<std::uint32_t>(thread.teb) == outer);
}
