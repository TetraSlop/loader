// advapi32's registry. Keys exist once created; the protector's probes of keys nobody made fail as on a clean
// machine. Values come from what the guest stored, then from tmloader's virtual registry, which the game reads
// under any key (src/platform/system.cpp: RegOpenKeyA and RegCreateKeyExA always succeed).
#include "hle/process.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace tmweb::hle
{
namespace
{

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

auto lower(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), [](const char c)
                           {
                               return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                           });
    return text;
}

// "hklm\software\..." for a root key or an open key plus a subkey; nothing for an unknown handle
auto keyPath(Process& p, const std::uint32_t parent, const std::string& subkey) -> std::optional<std::string>
{
    auto base = std::string{};
    switch (parent)
    {
        case 0x80000000:
            base = "hkcr";
            break;
        case 0x80000001:
            base = "hkcu";
            break;
        case 0x80000002:
            base = "hklm";
            break;
        case 0x80000003:
            base = "hku";
            break;
        default:
            if (const auto key = p.handleAs<Key>(parent))
            {
                base = key->path;
                break;
            }
            return std::nullopt;
    }
    auto path = lower(subkey);
    while (path.ends_with('\\'))
    {
        path.pop_back();
    }
    return path.empty() ? base : base + "\\" + path;
}

auto openKey(Process& p, const std::string& path) -> std::uint32_t
{
    auto key  = std::make_shared<Key>();
    key->path = path;
    return p.addHandle(key);
}

} // namespace

void installAdvapi32(Process& p)
{
    auto&      boundary = p.machine.boundary();
    auto&      memory   = p.machine.memory();
    const auto add      = [&](const char* name, const std::uint8_t argc, Handler handler = {})
    {
        boundary.add(Function{ "advapi32.dll", name, Convention::Stdcall, argc, std::move(handler) });
    };

    // RegOpenKeyA(key, subkey, result): the game's own import, always the virtual key
    add("RegOpenKeyA", 3, [&p, &memory](Call& call)
        {
            const auto path = keyPath(p, call.arg(0), call.argString(1));
            p.trace.push_back(std::format("RegOpenKeyA {}", path.value_or("?")));
            memory.write<std::uint32_t>(call.arg(2), openKey(p, path.value_or("")));
            return ret(kErrorSuccess);
        });
    // RegOpenKeyExA(key, subkey, options, sam, result)
    add("RegOpenKeyExA", 5, [&p, &memory](Call& call)
        {
            const auto path = keyPath(p, call.arg(0), call.argString(1));
            p.trace.push_back(std::format("RegOpenKeyExA {}", path.value_or("?")));
            if (!path || !p.registry.contains(*path))
            {
                return ret(kErrorFileNotFound);
            }
            memory.write<std::uint32_t>(call.arg(4), openKey(p, *path));
            return ret(kErrorSuccess);
        });
    // RegCreateKeyExA(key, subkey, reserved, class, options, sam, security, result, disposition)
    add("RegCreateKeyExA", 9, [&p, &memory](Call& call)
        {
            const auto path = keyPath(p, call.arg(0), call.argString(1));
            if (!path)
            {
                return ret(kErrorInvalidHandle);
            }
            const auto existed = p.registry.contains(*path);
            p.registry[*path];
            p.trace.push_back(std::format("RegCreateKeyExA {}", *path));
            memory.write<std::uint32_t>(call.arg(7), openKey(p, *path));
            if (call.arg(8))
            {
                memory.write<std::uint32_t>(call.arg(8), existed ? 2 : 1); // REG_OPENED_EXISTING_KEY, REG_CREATED_NEW_KEY
            }
            return ret(kErrorSuccess);
        });
    // RegQueryValueExA(key, name, reserved, type, data, length), with tmloader's registryValue rules
    add("RegQueryValueExA", 6, [&p, &memory](Call& call)
        {
            const auto key  = p.handleAs<Key>(call.arg(0));
            const auto name = lower(call.argString(1));
            p.trace.push_back(std::format("RegQueryValueExA {}", call.argString(1)));
            const auto* value = static_cast<const RegistryValue*>(nullptr);
            for (const auto& path : { key ? key->path : std::string{ "?" }, std::string{} })
            {
                const auto values = p.registry.find(path);
                if (values != p.registry.end() && values->second.contains(name))
                {
                    value = &values->second.at(name);
                    break;
                }
            }
            if (!value || call.argString(1).empty())
            {
                return ret(kErrorFileNotFound);
            }
            if (call.arg(3))
            {
                memory.write<std::uint32_t>(call.arg(3), value->type);
            }
            if (!call.arg(5))
            {
                return ret(kErrorInvalidParameter);
            }
            const auto capacity = memory.read<std::uint32_t>(call.arg(5));
            const auto size     = static_cast<std::uint32_t>(value->data.size());
            memory.write<std::uint32_t>(call.arg(5), size);
            if (!call.arg(4))
            {
                return ret(kErrorSuccess);
            }
            if (capacity < size)
            {
                return ret(kErrorMoreData);
            }
            memory.write(call.arg(4), std::span<const std::uint8_t>{ value->data });
            return ret(kErrorSuccess);
        });
    // RegSetValueExA(key, name, reserved, type, data, size)
    add("RegSetValueExA", 6, [&p, &memory](Call& call)
        {
            const auto key = p.handleAs<Key>(call.arg(0));
            if (!key)
            {
                return ret(kErrorInvalidHandle);
            }
            auto value = RegistryValue{ call.arg(3), std::vector<std::uint8_t>(call.arg(5)) };
            memory.read(call.arg(4), value.data);
            p.trace.push_back(std::format("RegSetValueExA {} {}", key->path, call.argString(1)));
            p.registry[key->path][lower(call.argString(1))] = std::move(value);
            return ret(kErrorSuccess);
        });
    add("RegCloseKey", 1, [&p](Call& call)
        {
            p.closeHandle(call.arg(0));
            return ret(kErrorSuccess);
        });
    add("RegFlushKey", 1, [](Call&)
        {
            return ret(kErrorSuccess);
        });
}

} // namespace tmweb::hle
