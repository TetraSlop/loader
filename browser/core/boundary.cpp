#include "core/boundary.h"

#include "core/machine.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <stdexcept>

namespace tmweb
{
namespace
{

constexpr std::uint32_t kImageAlignment = 0x10000; // module images start on 64 KiB, as LoadLibrary places them
constexpr std::uint32_t kSectionStart   = 0x1000;  // headers take the first page
constexpr std::uint32_t kSlotIndex      = 20;      // the slot's index is stored past the trap, where nothing runs
constexpr std::uint32_t kReturnTrapName = 0;       // the first late slot
// Module images take the first 1.5 MiB of the stand-in range (seventeen DLLs and the stand-in Viewer, each at least
// 64 KiB); slots made one at a time (COM methods, names resolved after publish, the return trap) the rest, 16,384
// of them, so they can be made before publish too
constexpr std::uint32_t kLateSlots = layout::kModules + 0x180000;

// mov edi,edi; push ebp; mov ebp,esp; pop ebp; nops up to the trap; int3 after it
constexpr auto kSlotBytes = []()
{
    auto bytes = std::array<std::uint8_t, TrapRange::kSlotSize>{};
    bytes.fill(0xCC);
    constexpr auto prologue = std::array<std::uint8_t, 6>{ 0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D };
    std::ranges::copy(prologue, bytes.begin());
    std::fill(bytes.begin() + prologue.size(), bytes.begin() + TrapRange::kTrapOffset, std::uint8_t{ 0x90 });
    return bytes;
}();

auto lower(std::string_view text) -> std::string
{
    auto out = std::string{ text };
    std::ranges::transform(out, out.begin(), [](const char c)
                           {
                               return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                           });
    return out;
}

auto key(const std::string_view module, const std::string_view name) -> std::string
{
    return std::format("{}!{}", lower(module), name);
}

auto align(const std::uint32_t value, const std::uint32_t alignment) -> std::uint32_t
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// "#11" is ordinal 11; anything else is a name
auto ordinalOf(const std::string_view name) -> std::uint32_t
{
    if (name.size() < 2 || name[0] != '#')
    {
        return 0;
    }
    auto value = std::uint32_t{};
    for (const auto c : name.substr(1))
    {
        if (c < '0' || c > '9')
        {
            return 0;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

// Bytes of one image, written at its RVAs and copied into guest memory at the end
class ImageWriter final
{
public:
    void u16(const std::uint32_t rva, const std::uint16_t value)
    {
        put(rva, &value, 2);
    }

    void u32(const std::uint32_t rva, const std::uint32_t value)
    {
        put(rva, &value, 4);
    }

    void text(const std::uint32_t rva, const std::string_view value)
    {
        put(rva, value.data(), static_cast<std::uint32_t>(value.size()));
        bytes_.resize(std::max<std::size_t>(bytes_.size(), rva + value.size() + 1));
    }

    void put(const std::uint32_t rva, const void* data, const std::uint32_t size)
    {
        bytes_.resize(std::max<std::size_t>(bytes_.size(), rva + size));
        std::memcpy(bytes_.data() + rva, data, size);
    }

    auto bytes() const -> const std::vector<std::uint8_t>&
    {
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

void writeSlot(Memory& memory, const std::uint32_t address, const std::uint32_t index)
{
    memory.write(address, std::span<const std::uint8_t>{ kSlotBytes });
    memory.write<std::uint32_t>(address + kSlotIndex, index);
}

} // namespace

Call::Call(Machine& machine, Context& context, const std::string_view name)
: machine_(machine)
, context_(context)
, name_(name)
{
}

auto Call::arg(const unsigned index) const -> std::uint32_t
{
    return machine_.memory().read<std::uint32_t>(context_.r[kEsp] + 4 + 4 * index);
}

auto Call::self() const -> std::uint32_t
{
    return context_.r[kEcx];
}

auto Call::argString(const unsigned index) const -> std::string
{
    const auto address = arg(index);
    if (address == 0)
    {
        return {};
    }
    return machine_.memory().readString(address);
}

auto Call::argWideString(const unsigned index) const -> std::u16string
{
    const auto address = arg(index);
    if (address == 0)
    {
        return {};
    }
    return machine_.memory().readWideString(address);
}

auto Call::returnAddress() const -> std::uint32_t
{
    return machine_.memory().read<std::uint32_t>(context_.r[kEsp]);
}

auto Call::name() const -> std::string_view
{
    return name_;
}

auto Call::machine() const -> Machine&
{
    return machine_;
}

auto Call::memory() const -> Memory&
{
    return machine_.memory();
}

auto Call::context() const -> Context&
{
    return context_;
}

Boundary::Boundary(Memory& memory)
: memory_(memory)
{
    next_ = kLateSlots;
    static_cast<void>(newSlot(Function{ "tmweb", std::to_string(kReturnTrapName), Convention::Cdecl, 0, [](Call&) -> Outcome
                                        {
                                            return Halt{ "the callGuest return trap ran outside callGuest" };
                                        } }));
}

void Boundary::add(Function function)
{
    if (published_)
    {
        throw std::logic_error("Boundary::add after publish");
    }
    function.module = lower(function.module);
    slots_.push_back(std::move(function));
}

// Each module is a PE image: headers in the first page, then one section holding the export directory and the
// slots. Images are 64 KiB aligned, so scanning down from an export for "MZ" finds the module's own header.
void Boundary::publish()
{
    if (published_)
    {
        throw std::logic_error("Boundary::publish twice");
    }

    // Functions add() registered; slots newSlot made already have their address
    const auto pending = [this](const Function& function)
    {
        return !entries_.contains(key(function.module, function.name));
    };
    auto modules = std::vector<std::string>{};
    for (const auto& function : slots_)
    {
        if (pending(function) && std::ranges::find(modules, function.module) == modules.end())
        {
            modules.push_back(function.module);
        }
    }

    auto base = layout::kModules;
    for (const auto& module : modules)
    {
        auto indices = std::vector<std::uint32_t>{};
        for (auto index = std::uint32_t{}; index < slots_.size(); ++index)
        {
            if (slots_[index].module == module && pending(slots_[index]))
            {
                indices.push_back(index);
            }
        }

        // Ordinals: "#n" keeps n, names fill the gaps from 1
        auto taken = std::set<std::uint32_t>{};
        for (const auto index : indices)
        {
            if (const auto ordinal = ordinalOf(slots_[index].name))
            {
                taken.insert(ordinal);
            }
        }
        auto ordinals = std::vector<std::uint32_t>(indices.size());
        auto next     = std::uint32_t{ 1 };
        for (auto i = std::size_t{}; i < indices.size(); ++i)
        {
            ordinals[i] = ordinalOf(slots_[indices[i]].name);
            if (ordinals[i] == 0)
            {
                while (taken.contains(next))
                {
                    ++next;
                }
                ordinals[i] = next;
                taken.insert(next);
            }
        }
        const auto functionCount = taken.empty() ? 0 : *taken.rbegin();

        // Names sorted by strcmp, as GetProcAddress's binary search expects
        auto named = std::vector<std::size_t>{};
        for (auto i = std::size_t{}; i < indices.size(); ++i)
        {
            if (ordinalOf(slots_[indices[i]].name) == 0)
            {
                named.push_back(i);
            }
        }
        std::ranges::sort(named, {}, [&](const std::size_t i) -> const std::string&
                          {
                              return slots_[indices[i]].name;
                          });

        auto       image          = ImageWriter{};
        const auto directory      = kSectionStart;
        const auto functionsTable = directory + 40;
        const auto namesTable     = functionsTable + 4 * functionCount;
        const auto ordinalsTable  = namesTable + 4 * static_cast<std::uint32_t>(named.size());
        auto       strings        = ordinalsTable + 2 * static_cast<std::uint32_t>(named.size());

        const auto moduleName = strings;
        image.text(moduleName, module);
        strings += static_cast<std::uint32_t>(module.size()) + 1;
        for (auto n = std::size_t{}; n < named.size(); ++n)
        {
            const auto& name = slots_[indices[named[n]]].name;
            image.u32(namesTable + 4 * static_cast<std::uint32_t>(n), strings);
            image.u16(ordinalsTable + 2 * static_cast<std::uint32_t>(n), static_cast<std::uint16_t>(ordinals[named[n]] - 1));
            image.text(strings, name);
            strings += static_cast<std::uint32_t>(name.size()) + 1;
        }
        const auto exportSize = strings - directory;

        auto slot = align(strings, TrapRange::kSlotSize);
        auto rvas = std::vector<std::uint32_t>(indices.size());
        for (auto i = std::size_t{}; i < indices.size(); ++i)
        {
            rvas[i] = slot;
            image.u32(functionsTable + 4 * (ordinals[i] - 1), slot);
            slot += TrapRange::kSlotSize;
        }
        const auto imageSize = align(slot, kPageSize);

        // IMAGE_EXPORT_DIRECTORY
        image.u32(directory + 12, moduleName);
        image.u32(directory + 16, 1); // ordinal base
        image.u32(directory + 20, functionCount);
        image.u32(directory + 24, static_cast<std::uint32_t>(named.size()));
        image.u32(directory + 28, functionsTable);
        image.u32(directory + 32, namesTable);
        image.u32(directory + 36, ordinalsTable);

        // DOS header, PE signature, file header, optional header, one section
        image.u16(0, 0x5A4D);
        image.u32(0x3C, 0x40);
        image.u32(0x40, 0x00004550);
        image.u16(0x44, 0x014C); // i386
        image.u16(0x46, 1);
        image.u16(0x54, 0xE0);
        image.u16(0x56, 0x2102); // executable, 32-bit, DLL
        constexpr auto optional = std::uint32_t{ 0x58 };
        image.u16(optional, 0x010B);
        image.u32(optional + 4, imageSize - kSectionStart); // SizeOfCode
        image.u32(optional + 20, kSectionStart);            // BaseOfCode
        image.u32(optional + 28, base);
        image.u32(optional + 32, kPageSize); // SectionAlignment
        image.u32(optional + 36, 0x200);     // FileAlignment
        image.u16(optional + 40, 5);         // OS 5.1
        image.u16(optional + 42, 1);
        image.u16(optional + 48, 4); // subsystem version 4.0
        image.u32(optional + 56, imageSize);
        image.u32(optional + 60, kSectionStart); // SizeOfHeaders
        image.u16(optional + 68, 3);             // console subsystem; unused for a DLL
        image.u32(optional + 92, 16);            // NumberOfRvaAndSizes
        image.u32(optional + 96, directory);
        image.u32(optional + 100, exportSize);
        constexpr auto section = optional + 0xE0;
        image.text(section, ".text");
        image.u32(section + 8, imageSize - kSectionStart);
        image.u32(section + 12, kSectionStart);
        image.u32(section + 16, imageSize - kSectionStart);
        image.u32(section + 20, kSectionStart);
        image.u32(section + 36, 0x60000020); // code, execute, read

        if (base + imageSize > kLateSlots)
        {
            throw std::runtime_error("Synthetic module images overflow their address range");
        }
        memory_.map(base, imageSize);
        memory_.write(base, std::span<const std::uint8_t>{ image.bytes() });
        for (auto i = std::size_t{}; i < indices.size(); ++i)
        {
            writeSlot(memory_, base + rvas[i], indices[i]);
            entries_[key(module, slots_[indices[i]].name)] = base + rvas[i];
        }
        modules_[module] = base;
        base             = align(base + imageSize, kImageAlignment);
    }

    published_ = true;
}

auto Boundary::module(const std::string_view name) const -> std::optional<std::uint32_t>
{
    const auto found = modules_.find(lower(name));
    if (found == modules_.end())
    {
        return std::nullopt;
    }
    return found->second;
}

auto Boundary::resolve(const std::string_view module, const std::string_view name) -> std::uint32_t
{
    const auto found = entries_.find(key(module, name));
    if (found != entries_.end())
    {
        return found->second;
    }
    if (!published_)
    {
        throw std::logic_error("Boundary::resolve of an unregistered function before publish");
    }
    return newSlot(Function{ lower(module), std::string{ name }, Convention::Stdcall, 0, {} });
}

auto Boundary::find(const std::uint32_t address) const -> const Function*
{
    const auto start = address & ~(TrapRange::kSlotSize - 1);
    if (start < layout::kModules || start >= layout::kModulesEnd || address - start > TrapRange::kTrapOffset ||
        !memory_.mapped(start, TrapRange::kSlotSize))
    {
        return nullptr;
    }
    const auto index = memory_.read<std::uint32_t>(start + kSlotIndex);
    if (index >= slots_.size())
    {
        return nullptr;
    }
    const auto& function = slots_[index];
    const auto  entry    = entries_.find(key(function.module, function.name));
    if (entry == entries_.end() || entry->second != start)
    {
        return nullptr;
    }
    return &function;
}

auto Boundary::addInterface(const std::string_view name, const std::span<const Method> methods) -> std::uint32_t
{
    const auto vtable = memory_.allocate(static_cast<std::uint32_t>(4 * methods.size()), 4);
    for (std::uint32_t index = 0; const auto& method : methods)
    {
        const auto entry = newSlot(Function{ std::string{ name }, method.name, Convention::Stdcall, method.argc, method.handler });
        memory_.write<std::uint32_t>(vtable + 4 * index++, entry);
    }
    return vtable;
}

auto Boundary::newObject(const std::uint32_t vtable, std::shared_ptr<void> host) -> std::uint32_t
{
    const auto object = memory_.allocate(8, 8);
    memory_.write<std::uint32_t>(object, vtable);
    memory_.write<std::uint32_t>(object + 4, static_cast<std::uint32_t>(objects_.size()));
    objects_.push_back(std::move(host));
    return object;
}

void Boundary::observe(std::function<void(const Call&)> observer)
{
    observer_ = std::move(observer);
}

// The trap itself, so a return there stops at once
auto Boundary::returnTrap() const -> std::uint32_t
{
    return entries_.at(key("tmweb", std::to_string(kReturnTrapName))) + TrapRange::kTrapOffset;
}

auto Boundary::traps() const -> TrapRange
{
    return TrapRange{ layout::kModules, layout::kModulesEnd - layout::kModules };
}

// The handler only: the machine completes the call (Machine::complete), at once for a Return, on wake for a Block
auto Boundary::dispatch(Machine& machine, Context& context, const std::uint32_t trap) -> Outcome
{
    const auto* function = find(trap);
    if (!function)
    {
        return Halt{ std::format("trap at {:#x} is not a stand-in", trap) };
    }
    const auto name = key(function->module, function->name);
    auto       call = Call{ machine, context, name };
    if (observer_)
    {
        observer_(call);
    }
    if (!function->handler)
    {
        return Halt{ std::format("unhandled import {} from {:#x}", name, call.returnAddress()) };
    }
    try
    {
        return function->handler(call);
    }
    catch (const GuestFault& fault)
    {
        // ponytail: halts; raising the access violation in the guest, as Windows does inside an API, can come later
        return Halt{ std::format("{} from {:#x} faulted on {:#x}", name, call.returnAddress(), fault.address) };
    }
}

auto Boundary::newSlot(Function function) -> std::uint32_t
{
    if (next_ + TrapRange::kSlotSize > layout::kModulesEnd)
    {
        throw std::runtime_error("Out of stand-in slots");
    }
    const auto address = next_;
    next_ += TrapRange::kSlotSize;
    memory_.map(address, TrapRange::kSlotSize);
    function.module                               = lower(function.module);
    entries_[key(function.module, function.name)] = address;
    slots_.push_back(std::move(function));
    writeSlot(memory_, address, static_cast<std::uint32_t>(slots_.size() - 1));
    return address;
}

// Null for a null or unmapped pointer or a cookie newObject never gave; the guest can forge the rest
auto Boundary::objectHost(const std::uint32_t guestObject) const -> void*
{
    if (guestObject == 0 || !memory_.mapped(guestObject, 8))
    {
        return nullptr;
    }
    const auto cookie = memory_.read<std::uint32_t>(guestObject + 4);
    return cookie < objects_.size() ? objects_[cookie].get() : nullptr;
}

} // namespace tmweb
