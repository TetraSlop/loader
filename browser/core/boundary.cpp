#include "core/boundary.h"

#include "core/machine.h"

#include <stdexcept>

namespace tmweb
{

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
}

void Boundary::add(Function function)
{
    slots_.push_back(std::move(function));
}

void Boundary::publish()
{
    published_ = true;
}

auto Boundary::module(const std::string_view name) const -> std::optional<std::uint32_t>
{
    const auto found = modules_.find(std::string{ name });
    if (found == modules_.end())
    {
        return std::nullopt;
    }
    return found->second;
}

auto Boundary::resolve(std::string_view, std::string_view) -> std::uint32_t
{
    throw std::logic_error("Boundary::resolve is not implemented yet");
}

auto Boundary::find(std::uint32_t) const -> const Function*
{
    return nullptr;
}

auto Boundary::addInterface(std::string_view, std::span<const Method>) -> std::uint32_t
{
    throw std::logic_error("Boundary::addInterface is not implemented yet");
}

auto Boundary::newObject(std::uint32_t, std::shared_ptr<void>) -> std::uint32_t
{
    throw std::logic_error("Boundary::newObject is not implemented yet");
}

void Boundary::observe(std::function<void(const Call&)> observer)
{
    observer_ = std::move(observer);
}

auto Boundary::returnTrap() const -> std::uint32_t
{
    return 0;
}

auto Boundary::traps() const -> TrapRange
{
    return TrapRange{ layout::kModules, layout::kModulesEnd - layout::kModules };
}

auto Boundary::dispatch(Machine&, Context&, std::uint32_t) -> Outcome
{
    return Halt{ "Boundary::dispatch is not implemented yet" };
}

auto Boundary::newSlot(Function) -> std::uint32_t
{
    return 0;
}

auto Boundary::objectHost(std::uint32_t) const -> void*
{
    return nullptr;
}

} // namespace tmweb
