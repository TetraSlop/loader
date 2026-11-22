#include "com/proxies.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

using namespace tmweb;

namespace
{

struct Guid
{
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t  data4[8];
};

struct Window;

struct IBase
{
    virtual long          QueryInterface(const Guid& iid, void** out) = 0;
    virtual unsigned long AddRef()                                    = 0;
    virtual unsigned long Release()                                   = 0;
};

struct Rect
{
    std::int32_t left, top, right, bottom;
};

struct ISurface : IBase
{
    virtual long Fill(const Rect* rect, float value, std::uint32_t color) = 0;
};

struct IResource : IBase
{
    virtual unsigned Priority() = 0;
};

struct ITexture : IResource
{
    virtual long Level(unsigned level, ISurface** surface) = 0;
};

struct IDevice : IBase
{
    virtual long CreateTexture(unsigned width, ITexture** texture) = 0;
    virtual long SetTexture(unsigned stage, IResource* texture)    = 0;
    virtual void SetWindow(Window* window)                         = 0;
    virtual long GetTexture(IResource** texture)                   = 0;
};

constexpr auto kSurfaceIid = Iid{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
constexpr auto kTextureIid = Iid{ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
constexpr auto kDeviceIid  = Iid{ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };

template <class I>
struct Counted : I
{
    unsigned long refs = 1;

    Counted()                                  = default;
    Counted(const Counted&)                    = delete;
    Counted(Counted&&)                         = delete;
    auto operator=(const Counted&) -> Counted& = delete;
    auto operator=(Counted&&) -> Counted&      = delete;
    virtual ~Counted()                         = default;

    long QueryInterface(const Guid&, void** out) override
    {
        *out = nullptr;
        return 0x80004002;
    }

    unsigned long AddRef() override
    {
        return ++refs;
    }

    unsigned long Release() override
    {
        const auto count = --refs;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }
};

struct Surface final : Counted<ISurface>
{
    Rect          rect{};
    float         value = 0;
    std::uint32_t color = 0;

    long Fill(const Rect* const r, const float v, const std::uint32_t c) override
    {
        rect  = r == nullptr ? Rect{ -1, -1, -1, -1 } : *r;
        value = v;
        color = c;
        return 0;
    }
};

struct Texture final : Counted<ITexture>
{
    Surface surface;

    unsigned Priority() override
    {
        return 7;
    }

    long Level(const unsigned level, ISurface** const out) override
    {
        if (level != 0)
        {
            return -1;
        }
        surface.AddRef();
        *out = &surface;
        return 0;
    }

    long QueryInterface(const Guid& iid, void** out) override
    {
        if (std::memcmp(&iid, kSurfaceIid.data(), 16) == 0)
        {
            surface.AddRef();
            *out = static_cast<ISurface*>(&surface);
            return 0;
        }
        return Counted::QueryInterface(iid, out);
    }
};

struct Device final : Counted<IDevice>
{
    IResource* bound  = nullptr;
    Window*    window = nullptr;

    long CreateTexture(unsigned, ITexture** const out) override
    {
        *out = new Texture;
        return 0;
    }

    long SetTexture(unsigned, IResource* const texture) override
    {
        bound = texture;
        return 0;
    }

    void SetWindow(Window* const w) override
    {
        window = w;
    }

    long GetTexture(IResource** const out) override
    {
        bound->AddRef();
        *out = bound;
        return 0;
    }
};

} // namespace

template <>
struct tmweb::Handle<Window> : std::true_type
{
};

namespace
{

// A guest caller: pushes this and the arguments under a return address, then calls the slot's handler as the
// boundary would on reaching its trap
auto publish(Machine& machine) -> bool
{
    machine.boundary().publish();
    return true;
}

struct Guest
{
    Machine       machine;
    bool          published = publish(machine); // COM slots come after the module images
    Proxies       proxies{ machine };
    Context       context{};
    std::uint32_t stack = machine.memory().allocate(0x1000);

    Guest()
    {
        proxies.add<ISurface>("ISurface", kSurfaceIid, { TMWEB_BIND(proxies, ISurface, Fill) });
        proxies.add<ITexture, IResource>("ITexture", kTextureIid, { TMWEB_BIND(proxies, ITexture, Priority), TMWEB_BIND(proxies, ITexture, Level) });
        proxies.add<IDevice>("IDevice", kDeviceIid, { TMWEB_BIND(proxies, IDevice, CreateTexture), TMWEB_BIND(proxies, IDevice, SetTexture), TMWEB_BIND(proxies, IDevice, SetWindow), TMWEB_BIND(proxies, IDevice, GetTexture) });
    }

    auto call(const std::uint32_t object, const unsigned slot, std::initializer_list<std::uint32_t> args) -> Outcome
    {
        auto& memory    = machine.memory();
        context.r[kEsp] = stack + 0x800;
        auto address    = context.r[kEsp];
        memory.write<std::uint32_t>(address, 0xDEADBEEF);
        memory.write<std::uint32_t>(address += 4, object);
        for (const auto arg : args)
        {
            memory.write<std::uint32_t>(address += 4, arg);
        }
        const auto* const method = proxies.slot(object, slot);
        REQUIRE(method != nullptr);
        CHECK(method->argc == 1 + args.size());
        auto call = Call{ machine, context, method->name };
        return method->handler(call);
    }

    auto eax(const std::uint32_t object, const unsigned slot, std::initializer_list<std::uint32_t> args = {})
        -> std::uint32_t
    {
        const auto outcome = call(object, slot, args);
        REQUIRE(std::holds_alternative<Return>(outcome));
        return std::get<Return>(outcome).eax;
    }

    auto scratch(const std::uint32_t size) -> std::uint32_t
    {
        return machine.memory().allocate(size);
    }
};

} // namespace

TEST_CASE("A host object has one proxy, which unwraps to it and to its bases")
{
    auto       guest   = Guest{};
    auto*      texture = new Texture;
    const auto proxy   = guest.proxies.wrap<ITexture>(texture);
    CHECK(proxy != 0);
    CHECK(guest.proxies.wrap<ITexture>(texture) == proxy);
    CHECK(guest.proxies.unwrap<ITexture>(proxy) == texture);
    CHECK(guest.proxies.unwrap<IResource>(proxy) == static_cast<IResource*>(texture));
    CHECK(guest.proxies.unwrap<ITexture>(0) == nullptr);
    CHECK_THROWS_AS(guest.proxies.unwrap<ISurface>(proxy), NotAProxy);
    CHECK_THROWS_AS(guest.proxies.unwrap<ITexture>(guest.scratch(8)), NotAProxy);
    CHECK(guest.proxies.wrap<ITexture>(nullptr) == 0);
    texture->Release();
}

TEST_CASE("Scalar, float and struct arguments reach the host method")
{
    auto       guest   = Guest{};
    auto*      surface = new Surface;
    const auto proxy   = guest.proxies.wrap<ISurface>(surface);
    const auto rect    = guest.scratch(sizeof(Rect));
    const auto value   = Rect{ 1, 2, 3, 4 };
    guest.machine.memory().write(rect, std::span{ reinterpret_cast<const std::uint8_t*>(&value), sizeof(value) });

    CHECK(guest.eax(proxy, 3, { rect, std::bit_cast<std::uint32_t>(0.5f), 0xFF00FF00 }) == 0);
    CHECK(surface->rect.left == 1);
    CHECK(surface->rect.bottom == 4);
    CHECK(surface->value == 0.5f);
    CHECK(surface->color == 0xFF00FF00);

    guest.eax(proxy, 3, { 0, 0, 0 });
    CHECK(surface->rect.left == -1);
    surface->Release();
}

TEST_CASE("Interfaces the host returns come back to the guest as proxies")
{
    auto       guest  = Guest{};
    auto*      device = new Device;
    const auto proxy  = guest.proxies.wrap<IDevice>(device);
    const auto out    = guest.scratch(4);

    CHECK(guest.eax(proxy, 3, { 640, out }) == 0);
    const auto texture = guest.machine.memory().read<std::uint32_t>(out);
    REQUIRE(texture != 0);
    CHECK(guest.eax(texture, 3) == 7);

    CHECK(guest.eax(texture, 4, { 0, out }) == 0);
    const auto surface = guest.machine.memory().read<std::uint32_t>(out);
    CHECK(guest.proxies.unwrap<ISurface>(surface) != nullptr);
    CHECK(guest.eax(texture, 4, { 0, out }) == 0);
    CHECK(guest.machine.memory().read<std::uint32_t>(out) == surface);

    CHECK(guest.eax(proxy, 4, { 0, texture }) == 0);
    CHECK(device->bound == guest.proxies.unwrap<IResource>(texture));
    CHECK(guest.eax(proxy, 6, { out }) == 0);
    CHECK(guest.machine.memory().read<std::uint32_t>(out) == texture);
    guest.eax(texture, 2);
    CHECK(guest.eax(proxy, 4, { 0, 0 }) == 0);
    CHECK(device->bound == nullptr);
    device->Release();
}

TEST_CASE("Anything but a proxy where an interface goes halts")
{
    auto       guest  = Guest{};
    auto*      device = new Device;
    const auto proxy  = guest.proxies.wrap<IDevice>(device);
    const auto bogus  = guest.scratch(8);

    CHECK(std::holds_alternative<Halt>(guest.call(proxy, 4, { 0, bogus })));
    CHECK(std::holds_alternative<Halt>(guest.call(proxy, 4, { 0, proxy })));
    device->Release();
}

TEST_CASE("Handles pass through as their value")
{
    auto       guest  = Guest{};
    auto*      device = new Device;
    const auto proxy  = guest.proxies.wrap<IDevice>(device);
    guest.eax(proxy, 5, { 0x00010092 });
    CHECK(reinterpret_cast<std::uintptr_t>(device->window) == 0x00010092);
    device->Release();
}

TEST_CASE("The last Release retires the proxy")
{
    auto       guest   = Guest{};
    auto*      texture = new Texture;
    const auto proxy   = guest.proxies.wrap<ITexture>(texture);
    CHECK(guest.eax(proxy, 1) == 2);
    CHECK(guest.eax(proxy, 2) == 1);
    CHECK(guest.eax(proxy, 2) == 0);
    CHECK_THROWS_AS(guest.proxies.unwrap<ITexture>(proxy), NotAProxy);
}

TEST_CASE("QueryInterface wraps what the host returns, by IID")
{
    auto       guest   = Guest{};
    auto*      texture = new Texture;
    const auto proxy   = guest.proxies.wrap<ITexture>(texture);
    const auto iid     = guest.scratch(16);
    const auto out     = guest.scratch(4);
    auto&      memory  = guest.machine.memory();

    memory.write(iid, std::span<const std::uint8_t>{ kSurfaceIid });
    CHECK(guest.eax(proxy, 0, { iid, out }) == 0);
    CHECK(guest.proxies.unwrap<ISurface>(memory.read<std::uint32_t>(out)) == &texture->surface);

    memory.write(iid, std::span<const std::uint8_t>{ Iid{ 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } });
    CHECK(guest.eax(proxy, 0, { iid, out }) == 0);
    CHECK(memory.read<std::uint32_t>(out) == proxy);
    CHECK(texture->refs == 2);

    memory.write(iid, std::span<const std::uint8_t>{ kDeviceIid });
    CHECK(guest.eax(proxy, 0, { iid, out }) == 0x80004002);
    CHECK(memory.read<std::uint32_t>(out) == 0);
    texture->Release();
    texture->Release();
}

TEST_CASE("Staging buffers are kept per host object part and grow")
{
    auto       guest = Guest{};
    auto       a     = 0;
    auto       b     = 0;
    const auto first = guest.proxies.staging(&a, 0, 64);
    CHECK(guest.proxies.staging(&a, 0, 32) == first);
    CHECK(guest.proxies.staging(&a, 1, 32) != first);
    CHECK(guest.proxies.staging(&b, 0, 32) != first);
    const auto grown = guest.proxies.staging(&a, 0, 128);
    CHECK(grown != first);
    guest.machine.memory().write<std::uint8_t>(grown + 127, 1);
}

TEST_CASE("A lock shows the guest the host's bytes and the unlock copies its writes back")
{
    auto       guest  = Guest{};
    auto       bytes  = std::array<std::uint8_t, 4>{ 1, 2, 3, 4 };
    auto&      memory = guest.machine.memory();
    const auto locked = guest.proxies.lock(&bytes, 0, bytes.data(), 4);
    CHECK(memory.read<std::uint32_t>(locked) == 0x04030201);
    memory.write<std::uint8_t>(locked + 2, 9);
    CHECK(bytes[2] == 3);
    CHECK(guest.proxies.unlock(&bytes, 0) == bytes.data());
    CHECK(bytes[2] == 9);
    CHECK(guest.proxies.unlock(&bytes, 0) == nullptr);
}
