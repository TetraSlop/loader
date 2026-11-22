#include "host/host.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace tmweb::host;

TEST_CASE("Input events update the state, then reach each listener in order")
{
    auto seen = std::vector<InputEvent>{};
    onInput([&seen](const InputEvent& event)
            {
                seen.push_back(event);
            });
    onInput(
        [](const InputEvent& event)
        {
            if (event.kind == InputKind::Key)
            {
                CHECK(inputState().keys[0x1C] == (event.b != 0));
            }
        });
    tmweb_input(0, 0x1C, 1);
    tmweb_input(1, 700, 100);
    tmweb_input(2, 1, 1);
    tmweb_input(0, 0x1C, 0);
    tmweb_input(0, 0x100, 1); // no such key: dropped
    REQUIRE(seen.size() == 4);
    CHECK(seen[0].a == 0x1C);
    CHECK(seen[1].kind == InputKind::Move);
    CHECK(seen[1].a == 639);
    CHECK(inputState().x == 639); // clamped to the client area
    CHECK(inputState().y == 100);
    CHECK(inputState().buttons[1]);
    CHECK_FALSE(inputState().keys[0x1C]);
}

TEST_CASE("Sound played is taken whole, as stereo frames")
{
    const auto frames = std::array{ 0.5f, -0.5f, 0.25f, -0.25f };
    playSound(frames);
    const auto* taken = tmweb_sound_take();
    REQUIRE(tmweb_sound_frames() == 2);
    CHECK(taken[2] == 0.25f);
    tmweb_sound_take();
    CHECK(tmweb_sound_frames() == 0);
}

TEST_CASE("A socket takes the transport's events and hands out what arrived")
{
    // Natively no address has a route
    CHECK(connect("127.0.0.1", 54230)->state() == SocketState::Failed);

    auto socket = Socket{ 1 };
    socket.opened();
    CHECK(socket.state() == SocketState::Open);
    const auto bytes = std::array<std::uint8_t, 3>{ 1, 2, 3 };
    socket.received(bytes);
    auto into = std::array<std::uint8_t, 2>{};
    CHECK(socket.receive(into) == 2);
    CHECK(into[1] == 2);
    CHECK(socket.available() == 1);
    socket.ended(SocketState::Closed);
    socket.opened();
    CHECK(socket.state() == SocketState::Closed);
    CHECK(socket.receive(into) == 1);
}

TEST_CASE("Booting needs a runtime folder")
{
    CHECK(tmweb_boot("no such folder", 0, "") != 0);
    CHECK(tmweb_run(1000) == 2);
}
