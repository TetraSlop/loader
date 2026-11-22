#include "hle/process.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>

namespace tmweb::hle
{
namespace
{

constexpr auto kModules = std::array{ "kernel32.dll", "user32.dll", "advapi32.dll", "imm32.dll", "ole32.dll", "oleaut32.dll", "gdi32.dll", "d3d8.dll", "dinput8.dll", "dsound.dll", "ntdll.dll" };

auto lower(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), [](const char c)
                           {
                               return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                           });
    return text;
}

auto roundBlock(const std::uint32_t size) -> std::uint32_t
{
    return (std::max(size, 1u) + 15) & ~15u;
}

} // namespace

auto Mutex::signaled(const ThreadId waiter) const -> bool
{
    return count == 0 || owner == waiter;
}

void Mutex::acquire(const ThreadId waiter)
{
    owner = waiter;
    ++count;
}

Process::Process(Machine& machine, HostFiles& files, std::string installRoot)
: machine(machine)
, files(files)
, installRoot(std::move(installRoot))
{
    const auto line = std::format("\"{}\\tmloader.exe\"", this->installRoot);
    commandLine     = allocate(static_cast<std::uint32_t>(line.size()) + 1);
    machine.memory().writeString(commandLine, line);
    environment = allocate(4); // an empty block: two terminators

    // tmloader's virtual registry (src/platform/system.cpp): served for every key the game opens
    const auto text = [](const std::string_view value)
    {
        auto data = std::vector<std::uint8_t>(value.begin(), value.end());
        data.push_back(0);
        return RegistryValue{ 1, std::move(data) }; // REG_SZ
    };
    const auto dword = [](const std::uint32_t value)
    {
        return RegistryValue{ 4, { static_cast<std::uint8_t>(value), 0, 0, 0 } }; // REG_DWORD
    };
    auto& virtualValues      = registry[""];
    virtualValues["loaddir"] = text(""); // an empty prefix avoids the archive-name parser's first-dot handling
    for (const auto* name : { "0002", "path", "installfolder" })
    {
        virtualValues[name] = text(this->installRoot);
    }
    virtualValues["sound"]      = dword(1); // SOUND=0 would skip the sound engine
    virtualValues["fullscreen"] = dword(0);
}

auto Process::addHandle(std::shared_ptr<Waitable> object) -> std::uint32_t
{
    const auto value = nextHandle_;
    nextHandle_ += 4;
    handles_[value] = std::move(object);
    return value;
}

auto Process::handle(const std::uint32_t value) const -> std::shared_ptr<Waitable>
{
    const auto found = handles_.find(value);
    return found == handles_.end() ? nullptr : found->second;
}

auto Process::closeHandle(const std::uint32_t value) -> bool
{
    return handles_.erase(value) != 0;
}

// ponytail: exact-size free lists, no coalescing; a real allocator if fragmentation ever exhausts the arena
auto Process::allocate(const std::uint32_t size) -> std::uint32_t
{
    auto&      memory  = machine.memory();
    const auto rounded = roundBlock(size);
    auto&      list    = freeLists_[rounded];
    auto       address = std::uint32_t{};
    if (list.empty())
    {
        address = memory.allocate(rounded, 16);
    }
    else
    {
        address = list.back();
        list.pop_back();
        memory.fill(address, 0, rounded);
    }
    blocks_[address] = size;
    return address;
}

void Process::free(const std::uint32_t address)
{
    const auto found = blocks_.find(address);
    if (found == blocks_.end())
    {
        return;
    }
    freeLists_[roundBlock(found->second)].push_back(address);
    blocks_.erase(found);
}

auto Process::blockSize(const std::uint32_t address) const -> std::optional<std::uint32_t>
{
    const auto found = blocks_.find(address);
    if (found == blocks_.end())
    {
        return std::nullopt;
    }
    return found->second;
}

auto Process::thread() -> Thread&
{
    return machine.scheduler().current();
}

void Process::setLastError(const std::uint32_t error)
{
    machine.memory().write<std::uint32_t>(thread().teb + 0x34, error);
}

auto Process::hostPath(const std::string_view guestPath) const -> std::optional<std::string>
{
    auto path = std::string{ guestPath };
    std::ranges::replace(path, '\\', '/');
    if (path.size() >= 2 && path[1] == ':')
    {
        auto root = lower(installRoot);
        std::ranges::replace(root, '\\', '/');
        const auto folded = lower(path);
        if (!folded.starts_with(root) || (folded.size() > root.size() && folded[root.size()] != '/'))
        {
            return std::nullopt;
        }
        path.erase(0, root.size());
    }
    else if (path.starts_with('/'))
    {
        return std::nullopt; // the root of the current drive, outside the folder
    }

    auto parts = std::vector<std::string>{};
    for (const auto part : std::views::split(path, '/'))
    {
        const auto name = std::string{ part.begin(), part.end() };
        if (name.empty() || name == ".")
        {
            continue;
        }
        if (name == "..")
        {
            if (parts.empty())
            {
                return std::nullopt;
            }
            parts.pop_back();
            continue;
        }
        parts.push_back(name);
    }
    auto out = std::string{};
    for (const auto& part : parts)
    {
        out += out.empty() ? part : "/" + part;
    }
    return out;
}

auto Process::writable(const std::string_view path) -> bool
{
    const auto top = lower(std::string{ path.substr(0, path.find('/')) });
    return path.find('/') != std::string_view::npos && (top == "pub" || top == "logs" || top == "settings");
}

auto Process::flush(File& file) -> bool
{
    if (!file.dirty)
    {
        return true;
    }
    file.dirty = false;
    return files.write(file.path, *file.data);
}

void install(Process& process)
{
    installKernel32(process);
    installUser32(process);
    installAdvapi32(process);
    installSmallModules(process);
}

void finishInstall(Process& process)
{
    for (const auto* name : kModules)
    {
        if (const auto base = process.machine.boundary().module(name))
        {
            process.modules[*base] = name;
        }
    }
}

} // namespace tmweb::hle
