#include "core/machine.h"
#include "core/threads.h"

#include <catch2/catch_test_macros.hpp>

using namespace tmweb;

namespace
{

class Signal final : public Waitable
{
public:
    auto signaled(ThreadId) const -> bool override
    {
        return set;
    }

    void acquire(ThreadId) override
    {
        ++taken;
    }

    bool set   = false;
    int  taken = 0;
};

} // namespace

TEST_CASE("Threads get a TEB on FS, a stack and their start routine")
{
    auto  memory    = Memory{};
    auto  scheduler = Scheduler{ memory };
    auto& main      = scheduler.createMain(0x10000);
    auto& worker    = scheduler.create(0x10001234, 0x55, 0x20000, false, 0x10A00010);
    CHECK(worker.context.fsBase == worker.teb);
    CHECK(memory.read<std::uint32_t>(worker.teb) == 0xFFFFFFFF);
    CHECK(memory.read<std::uint32_t>(worker.teb + 0x18) == worker.teb);
    CHECK(memory.read<std::uint32_t>(worker.teb + 0x24) == worker.id);
    CHECK(memory.read<std::uint32_t>(worker.teb + 4) == worker.stackBase);
    CHECK(worker.stackBase - worker.stackLimit == 0x20000);
    CHECK(worker.context.eip == 0x10001234);
    CHECK(memory.read<std::uint32_t>(worker.context.r[kEsp]) == 0x10A00010);
    CHECK(memory.read<std::uint32_t>(worker.context.r[kEsp] + 4) == 0x55);
    CHECK(worker.teb > main.stackBase); // a guard page between them
    CHECK_FALSE(memory.mapped(main.stackBase));
}

TEST_CASE("The scheduler takes turns and skips threads that cannot run")
{
    auto  memory    = Memory{};
    auto  scheduler = Scheduler{ memory };
    auto& a         = scheduler.createMain(0x10000);
    auto& b         = scheduler.create(0, 0, 0x10000, false, 0);
    auto& c         = scheduler.create(0, 0, 0x10000, true, 0);
    REQUIRE(scheduler.schedule());
    CHECK(&scheduler.current() == &b);
    REQUIRE(scheduler.schedule());
    CHECK(&scheduler.current() == &a); // c is suspended
    scheduler.resume(c);
    REQUIRE(scheduler.schedule());
    CHECK(&scheduler.current() == &b);
    REQUIRE(scheduler.schedule());
    CHECK(&scheduler.current() == &c);
    scheduler.exit(b, 3);
    CHECK(b.handle->signaled(a.id));
    REQUIRE(scheduler.schedule());
    CHECK(&scheduler.current() == &a);
}

TEST_CASE("A blocked thread wakes when its object is signaled, and acquires it")
{
    auto  memory    = Memory{};
    auto  scheduler = Scheduler{ memory };
    auto& main      = scheduler.createMain(0x10000);
    auto  flag      = std::make_shared<Signal>();
    auto  woke      = std::optional<WaitResult>{};
    scheduler.block(Wait{ { flag }, false, kInfinite }, [&woke](const WaitResult& result)
                    {
                        woke = result;
                    });
    CHECK(main.state == ThreadState::Waiting);
    CHECK_FALSE(scheduler.schedule());
    CHECK(scheduler.waiting() == 1);
    CHECK_FALSE(scheduler.nextDeadline());
    flag->set = true;
    REQUIRE(scheduler.schedule());
    REQUIRE(woke);
    CHECK_FALSE(woke->timedOut);
    CHECK(flag->taken == 1);
    CHECK(main.state == ThreadState::Ready);
}

TEST_CASE("Waits time out on the scheduler's clock; a wait on all needs every object")
{
    auto memory    = Memory{};
    auto scheduler = Scheduler{ memory };
    auto now       = std::uint64_t{ 1000 };
    scheduler.setClock([&now]()
                       {
                           return now;
                       });
    scheduler.createMain(0x10000);
    auto first  = std::make_shared<Signal>();
    auto second = std::make_shared<Signal>();
    auto woke   = std::optional<WaitResult>{};
    scheduler.block(Wait{ { first, second }, true, 50 }, [&woke](const WaitResult& result)
                    {
                        woke = result;
                    });
    CHECK(scheduler.nextDeadline() == 1050);
    first->set = true;
    CHECK_FALSE(scheduler.schedule());
    now = 1050;
    REQUIRE(scheduler.schedule());
    REQUIRE(woke);
    CHECK(woke->timedOut);
    CHECK(first->taken == 0);
}

TEST_CASE("A wait on any reports which object woke it")
{
    auto memory    = Memory{};
    auto scheduler = Scheduler{ memory };
    scheduler.createMain(0x10000);
    auto first  = std::make_shared<Signal>();
    auto second = std::make_shared<Signal>();
    second->set = true;
    auto woke   = std::optional<WaitResult>{};
    scheduler.block(Wait{ { first, second }, false, kInfinite }, [&woke](const WaitResult& result)
                    {
                        woke = result;
                    });
    REQUIRE(scheduler.schedule());
    REQUIRE(woke);
    CHECK(woke->index == 1);
    CHECK(second->taken == 1);
}

TEST_CASE("The machine runs a thread for its budget and counts per call")
{
    auto  machine = Machine{};
    auto& main    = machine.scheduler().createMain(0x10000);
    machine.memory().map(layout::kHeap, 0x1000);
    machine.memory().write<std::uint8_t>(layout::kHeap, 0x40);     // inc eax
    machine.memory().write<std::uint8_t>(layout::kHeap + 1, 0xEB); // jmp $-3
    machine.memory().write<std::uint8_t>(layout::kHeap + 2, 0xFD);
    main.context.eip = layout::kHeap;
    auto result      = machine.run(1001);
    CHECK_FALSE(result.halted);
    CHECK(result.instructions == 1001);
    CHECK(main.context.r[kEax] == 501);
    result = machine.run(10);
    CHECK(result.instructions == 10);
    CHECK(machine.cpu().retired() == 1011);
}

TEST_CASE("A fault with no handler halts the machine and says where")
{
    auto  machine = Machine{};
    auto& main    = machine.scheduler().createMain(0x10000);
    machine.memory().map(layout::kHeap, 0x1000);
    machine.memory().write<std::uint16_t>(layout::kHeap, 0x0031); // xor [eax], eax
    main.context.eip  = layout::kHeap;
    const auto result = machine.run(100);
    CHECK(result.halted);
    CHECK(result.reason == "Unhandled access violation at 0x11000000 reading 0x0");
}
