#pragma once

// Guest proxies for host COM objects. The game calls COM methods through x86 vtables, stdcall with this first. Each
// interface gets one guest vtable (Boundary::addInterface), and each host object handed to the guest gets a guest
// object with that vtable (Boundary::newObject). Slots 0 to 2 are IUnknown's and come for free; bind<I, &I::Method>
// makes the rest from the method's C++ type, one 32-bit stack argument per parameter:
//
// - integers and enums by value, floats by their bits;
// - an interface pointer is unwrapped to the host object, and must be a proxy;
// - an interface out-pointer (I**) gets a host slot, and after the call the guest gets the host object's proxy;
// - a handle type (Handle<T>) passes its 32-bit value through;
// - any other pointer or reference points into guest memory. In the Wasm build guest addresses are host addresses,
//   so structs pass in place, pointers inside them included.
//
// Methods that need more (Lock's staging buffers, callbacks into guest code) are written by hand as plain Methods.

#include "core/boundary.h"
#include "core/machine.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tmweb
{

using Iid = std::array<std::uint8_t, 16>;

// Handles the guest only stores and passes back, such as HWND; the bindings specialise this for their types
template <class T>
struct Handle : std::false_type
{
};

// The guest passed something other than a proxy where an interface goes
struct NotAProxy
{
    std::uint32_t address = 0;
};

class Proxies final
{
public:
    explicit Proxies(Machine& machine);

    // Register interface I and the interfaces it derives from that a parameter may name (Bases), with the methods
    // after IUnknown's in vtable order. Returns the vtable.
    template <class I, class... Bases>
    auto add(std::string_view name, const Iid& iid, std::vector<Method> methods) -> std::uint32_t;

    // The guest object for a host object, the same one each time; 0 for null. For an interface with no vtable of
    // its own (IDirect3DBaseTexture8), the proxy the object already has through a derived one.
    template <class I>
    auto wrap(I* host) -> std::uint32_t
    {
        if (host == nullptr)
        {
            return 0;
        }
        if (const auto* const interface = find(typeid(I)))
        {
            return wrap(*interface, host);
        }
        return existing(typeid(I), host);
    }

    // A proxy for what a QueryInterface-like method returned as iid; 0 when no interface has that IID
    auto wrap(const Iid& iid, void* host) -> std::uint32_t;

    // The host object behind a proxy, as I or one of its bases; null for 0. Throws NotAProxy for anything else.
    template <class I>
    auto unwrap(std::uint32_t guest) const -> I*
    {
        return static_cast<I*>(unwrap(guest, typeid(I)));
    }

    // unwrap for this: null is not a proxy either
    template <class I>
    auto self(const Call& call) const -> I*
    {
        auto* const host = unwrap<I>(call.arg(0));
        if (host == nullptr)
        {
            throw NotAProxy{ 0 };
        }
        return host;
    }

    // The host object went away: its proxies stop unwrapping
    void forget(const void* host);

    // A guest buffer of at least size bytes until it is given back
    auto borrow(std::uint32_t size) -> std::uint32_t;
    void giveBack(std::uint32_t address);

    // Lock: the guest cannot touch host memory, so it gets the locked bytes in a borrowed buffer, and unlock copies
    // them back and returns the buffer. sub tells a host object's parts apart (a texture's levels). unlock returns
    // the host bytes, or null when nothing is locked.
    auto lock(const void* host, std::uint32_t sub, std::uint8_t* bytes, std::uint32_t size) -> std::uint32_t;
    auto unlock(const void* host, std::uint32_t sub) -> std::uint8_t*;

    // The method behind a guest object's vtable slot, for tests and tracing; null past the end
    auto slot(std::uint32_t guest, unsigned index) const -> const Method*;

    auto machine() const -> Machine&
    {
        return machine_;
    }

    auto memory() const -> Memory&
    {
        return machine_.memory();
    }

private:
    using Cast = void* (*)(void*);

    struct Interface
    {
        std::string                                   name;
        Iid                                           iid{};
        std::type_index                               type;
        std::uint32_t                                 vtable = 0;
        std::vector<std::pair<std::type_index, Cast>> views; // I itself first
        std::vector<Method>                           methods;
    };

    struct Entry
    {
        void*            host      = nullptr; // as the interface's own type
        const Interface* interface = nullptr;
    };

    auto addInterface(Interface interface, std::vector<Method> methods) -> std::uint32_t;
    auto find(std::type_index type) const -> const Interface*;
    auto existing(std::type_index type, const void* host) const -> std::uint32_t;
    auto wrap(const Interface& interface, void* host) -> std::uint32_t;
    auto unwrap(std::uint32_t guest, std::type_index type) const -> void*;

    template <class I>
    auto unknownMethods() -> std::vector<Method>;

    Machine&                                                          machine_;
    std::vector<std::unique_ptr<Interface>>                           interfaces_;
    std::unordered_map<std::uint32_t, Entry>                          guests_;
    std::map<std::pair<const void*, const Interface*>, std::uint32_t> hosts_;
    using Part = std::pair<const void*, std::uint32_t>;

    struct Locked
    {
        std::uint8_t* bytes = nullptr;
        std::uint32_t guest = 0;
        std::uint32_t size  = 0;
    };

    std::map<Part, Locked> locked_;
};

namespace detail
{

template <class>
struct MemberTraits;

template <class R, class C, class... A>
struct MemberTraits<R (C::*)(A...)>
{
    using Class  = C;
    using Result = R;
    using Args   = std::tuple<A...>;
};

template <class T>
constexpr bool kInterface = std::is_class_v<T> && std::is_polymorphic_v<T>;

// One parameter's value for the call, and what to write back after it
template <class T>
struct Value
{
    T value;

    Value(Proxies& proxies, const std::uint32_t raw)
    : value(translate(proxies, raw))
    {
    }

    operator T() const
    {
        return value;
    }

    void finish(Proxies&)
    {
    }

    static auto translate(Proxies& proxies, const std::uint32_t raw) -> T
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            static_assert(sizeof(T) == 4);
            return std::bit_cast<T>(raw);
        }
        else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
        {
            static_assert(sizeof(T) <= 4, "64-bit arguments take two stack slots; write the method by hand");
            return static_cast<T>(raw);
        }
        else if constexpr (std::is_reference_v<T>)
        {
            // DirectInput's property GUIDs are small integers cast to references; nothing reads them
            using U = std::remove_reference_t<T>;
            if (raw >= 0x10000)
            {
                proxies.memory().check(raw, sizeof(U), Access::Read);
            }
            return *reinterpret_cast<U*>(proxies.memory().host(raw));
        }
        else
        {
            static_assert(std::is_pointer_v<T>);
            using U = std::remove_cv_t<std::remove_pointer_t<T>>;
            static_assert(!std::is_function_v<U>, "a callback into guest code needs a hand-written method");
            static_assert(!std::is_pointer_v<U>, "the host would write a host pointer for the guest; write the method by hand");
            if constexpr (Handle<U>::value)
            {
                return reinterpret_cast<T>(static_cast<std::uintptr_t>(raw));
            }
            else if constexpr (kInterface<U>)
            {
                return proxies.unwrap<U>(raw);
            }
            else
            {
                if (raw == 0)
                {
                    return nullptr;
                }
                if constexpr (!std::is_void_v<U>)
                {
                    proxies.memory().check(raw, sizeof(U), Access::Read);
                }
                return reinterpret_cast<T>(proxies.memory().host(raw));
            }
        }
    }
};

// I**: the host writes its object here, and the guest gets its proxy
template <class I>
    requires kInterface<I>
struct Value<I**>
{
    std::uint32_t guest = 0;
    I*            host  = nullptr;

    Value(Proxies& proxies, const std::uint32_t raw)
    : guest(raw)
    {
        if (raw != 0)
        {
            proxies.memory().check(raw, 4, Access::Write);
        }
    }

    operator I**()
    {
        return guest == 0 ? nullptr : &host;
    }

    void finish(Proxies& proxies)
    {
        if (guest != 0)
        {
            proxies.memory().write<std::uint32_t>(guest, proxies.wrap(host));
        }
    }
};

template <class R>
auto result(const R value) -> std::uint32_t
{
    if constexpr (std::is_pointer_v<R>)
    {
        static_assert(Handle<std::remove_cv_t<std::remove_pointer_t<R>>>::value, "only handles come back as pointers");
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(value));
    }
    else
    {
        return static_cast<std::uint32_t>(value);
    }
}

template <class I, auto M, class... A, std::size_t... Index>
auto invoke(Proxies& proxies, Call& call, std::index_sequence<Index...>) -> Return
{
    using C          = typename MemberTraits<decltype(M)>::Class;
    using R          = typename MemberTraits<decltype(M)>::Result;
    auto* const self = static_cast<C*>(proxies.self<I>(call));
    // Braced initialisation runs left to right, as the arguments lie on the stack
    auto values = std::tuple<Value<A>...>{ Value<A>(proxies, call.arg(1 + Index))... };
    auto eax    = std::uint32_t{ 0 };
    if constexpr (std::is_void_v<R>)
    {
        (self->*M)(static_cast<A>(std::get<Index>(values))...);
    }
    else
    {
        eax = result((self->*M)(static_cast<A>(std::get<Index>(values))...));
    }
    (std::get<Index>(values).finish(proxies), ...);
    return Return{ eax };
}

template <class I, auto M, class Args>
struct Binder;

template <class I, auto M, class... A>
struct Binder<I, M, std::tuple<A...>>
{
    static constexpr auto kArgc = static_cast<std::uint8_t>(1 + sizeof...(A));

    static auto call(Proxies& proxies, Call& call) -> Return
    {
        return invoke<I, M, A...>(proxies, call, std::index_sequence_for<A...>{});
    }
};

} // namespace detail

// Halts the machine when the guest passes a non-proxy where an interface goes
template <class F>
auto guarded(Call& call, F&& body) -> Outcome
{
    try
    {
        return body();
    }
    catch (const NotAProxy& error)
    {
        return Halt{ std::format("{}: {:#x} is not a proxy", call.name(), error.address) };
    }
}

// A vtable slot that calls I's method M on the host object
template <class I, auto M>
auto bind(Proxies& proxies, std::string name) -> Method
{
    using Binder = detail::Binder<I, M, typename detail::MemberTraits<decltype(M)>::Args>;
    return Method{ std::move(name), Binder::kArgc, [&proxies](Call& call) -> Outcome
                   {
                       return guarded(call, [&]() -> Outcome
                                      {
                                          return Binder::call(proxies, call);
                                      });
                   } };
}

// bind with the method's own name: TMWEB_BIND(proxies, IDirect3DDevice8, Present)
#define TMWEB_BIND(proxies, I, M) ::tmweb::bind<I, &I::M>(proxies, #M)

template <class I>
auto Proxies::unknownMethods() -> std::vector<Method>
{
    auto methods = std::vector<Method>{};
    methods.push_back(Method{ "QueryInterface", 3, [this](Call& call) -> Outcome
                              {
                                  return guarded(call,
                                                 [&]() -> Outcome
                                                 {
                                                     auto* const self = this->self<I>(call);
                                                     const auto  out  = call.arg(2);
                                                     auto        iid  = Iid{};
                                                     memory().read(call.arg(1), iid);
                                                     memory().write<std::uint32_t>(out, 0);
                                                     constexpr auto kUnknown = Iid{ 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
                                                     if (iid == kUnknown)
                                                     {
                                                         self->AddRef();
                                                         memory().write<std::uint32_t>(out, call.arg(0));
                                                         return Return{ 0 };
                                                     }
                                                     using Riid      = std::tuple_element_t<0, typename detail::MemberTraits<decltype(&I::QueryInterface)>::Args>;
                                                     auto*      host = static_cast<void*>(nullptr);
                                                     const auto code = static_cast<std::uint32_t>(self->QueryInterface(*reinterpret_cast<std::remove_reference_t<Riid>*>(memory().host(call.arg(1))), &host));
                                                     if (code != 0)
                                                     {
                                                         return Return{ code };
                                                     }
                                                     const auto guest = wrap(iid, host);
                                                     if (guest == 0)
                                                     {
                                                         // The same object, so the same count: no proxy for that interface
                                                         self->Release();
                                                         return Return{ 0x80004002 }; // E_NOINTERFACE
                                                     }
                                                     memory().write<std::uint32_t>(out, guest);
                                                     return Return{ 0 };
                                                 });
                              } });
    methods.push_back(Method{ "AddRef", 1, [this](Call& call) -> Outcome
                              {
                                  return guarded(call, [&]() -> Outcome
                                                 {
                                                     return Return{ static_cast<std::uint32_t>(self<I>(call)->AddRef()) };
                                                 });
                              } });
    methods.push_back(Method{ "Release", 1, [this](Call& call) -> Outcome
                              {
                                  return guarded(call,
                                                 [&]() -> Outcome
                                                 {
                                                     auto* const self  = this->self<I>(call);
                                                     const auto  count = static_cast<std::uint32_t>(self->Release());
                                                     if (count == 0)
                                                     {
                                                         forget(self);
                                                     }
                                                     return Return{ count };
                                                 });
                              } });
    return methods;
}

template <class I, class... Bases>
auto Proxies::add(const std::string_view name, const Iid& iid, std::vector<Method> methods) -> std::uint32_t
{
    auto interface = Interface{ std::string{ name }, iid, typeid(I), 0, {}, {} };
    interface.views.emplace_back(typeid(I), [](void* host) -> void*
                                 {
                                     return host;
                                 });
    (interface.views.emplace_back(typeid(Bases), [](void* host) -> void*
                                  {
                                      return static_cast<Bases*>(static_cast<I*>(host));
                                  }),
     ...);
    auto all = unknownMethods<I>();
    all.insert(all.end(), std::make_move_iterator(methods.begin()), std::make_move_iterator(methods.end()));
    return addInterface(std::move(interface), std::move(all));
}

} // namespace tmweb
