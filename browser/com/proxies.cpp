#include "com/proxies.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace tmweb
{

Proxies::Proxies(Machine& machine)
: machine_(machine)
{
}

auto Proxies::addInterface(Interface interface, std::vector<Method> methods) -> std::uint32_t
{
    interface.vtable  = machine_.boundary().addInterface(interface.name, methods);
    interface.methods = std::move(methods);
    interfaces_.push_back(std::make_unique<Interface>(std::move(interface)));
    return interfaces_.back()->vtable;
}

auto Proxies::find(const std::type_index type) const -> const Interface*
{
    for (const auto& interface : interfaces_)
    {
        if (interface->type == type)
        {
            return interface.get();
        }
    }
    return nullptr;
}

auto Proxies::existing(const std::type_index type, const void* const host) const -> std::uint32_t
{
    for (const auto& [guest, entry] : guests_)
    {
        for (const auto& [view, cast] : entry.interface->views)
        {
            if (view == type && cast(entry.host) == host)
            {
                return guest;
            }
        }
    }
    throw std::logic_error(std::string{ "no proxy to return as " } + type.name());
}

auto Proxies::wrap(const Iid& iid, void* const host) -> std::uint32_t
{
    for (const auto& interface : interfaces_)
    {
        if (interface->iid == iid)
        {
            return wrap(*interface, host);
        }
    }
    return 0;
}

auto Proxies::wrap(const Interface& interface, void* const host) -> std::uint32_t
{
    if (host == nullptr)
    {
        return 0;
    }
    const auto key = std::pair<const void*, const Interface*>{ host, &interface };
    if (const auto found = hosts_.find(key); found != hosts_.end())
    {
        return found->second;
    }
    // COM reference counts keep the host object alive, not the proxy
    const auto guest = machine_.boundary().newObject(interface.vtable, std::shared_ptr<void>(host, [](void*)
                                                                                             {
                                                                                             }));
    hosts_.emplace(key, guest);
    guests_.emplace(guest, Entry{ host, &interface });
    return guest;
}

auto Proxies::unwrap(const std::uint32_t guest, const std::type_index type) const -> void*
{
    if (guest == 0)
    {
        return nullptr;
    }
    const auto found = guests_.find(guest);
    if (found == guests_.end())
    {
        throw NotAProxy{ guest };
    }
    for (const auto& [view, cast] : found->second.interface->views)
    {
        if (view == type)
        {
            return cast(found->second.host);
        }
    }
    throw NotAProxy{ guest };
}

void Proxies::forget(const void* const host)
{
    std::erase_if(hosts_, [host](const auto& item)
                  {
                      return item.first.first == host;
                  });
    std::erase_if(guests_, [host](const auto& item)
                  {
                      return item.second.host == host;
                  });
    std::erase_if(locked_, [this, host](const auto& item)
                  {
                      if (item.first.first != host)
                      {
                          return false;
                      }
                      giveBack(item.second.guest);
                      return true;
                  });
}

auto Proxies::borrow(const std::uint32_t size) -> std::uint32_t
{
    return machine_.memory().allocate(std::max<std::uint32_t>(size, 1));
}

void Proxies::giveBack(const std::uint32_t address)
{
    machine_.memory().free(address);
}

auto Proxies::lock(const void* const host, const std::uint32_t sub, std::uint8_t* const bytes, const std::uint32_t size)
    -> std::uint32_t
{
    // A lock the guest never unlocked gives its buffer back first
    unlock(host, sub);
    const auto guest = borrow(size);
    std::memcpy(machine_.memory().host(guest), bytes, size);
    locked_[{ host, sub }] = Locked{ bytes, guest, size };
    return guest;
}

auto Proxies::unlock(const void* const host, const std::uint32_t sub) -> std::uint8_t*
{
    const auto found = locked_.find({ host, sub });
    if (found == locked_.end())
    {
        return nullptr;
    }
    const auto [bytes, guest, size] = found->second;
    locked_.erase(found);
    std::memcpy(bytes, machine_.memory().host(guest), size);
    giveBack(guest);
    return bytes;
}

auto Proxies::slot(const std::uint32_t guest, const unsigned index) const -> const Method*
{
    const auto found = guests_.find(guest);
    if (found == guests_.end() || index >= found->second.interface->methods.size())
    {
        return nullptr;
    }
    return &found->second.interface->methods[index];
}

} // namespace tmweb
