#include "host/host.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace tmweb::host;

TEST_CASE("Input events come out once, in the order the host sent them")
{
    tmweb_input(0, 0x1C, 1);
    tmweb_input(1, 320, 240);
    const auto events = takeInput();
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == InputKind::Key);
    CHECK(events[0].a == 0x1C);
    CHECK(events[1].kind == InputKind::Move);
    CHECK(events[1].b == 240);
    CHECK(takeInput().empty());
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
