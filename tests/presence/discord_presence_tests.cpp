#include "presence/discord_presence.h"
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <nlohmann/json.hpp>
#include <thread>

namespace tmloader
{

void writeLog(std::string_view)
{
}

} // namespace tmloader

using namespace tmloader;

namespace
{

// Reads one frame from the loader and answers it, the way the Discord client does.
auto exchange(HANDLE pipe) -> std::pair<std::uint32_t, nlohmann::json>
{
    auto header = std::array<std::uint32_t, 2>{};
    auto read   = DWORD{};
    REQUIRE(ReadFile(pipe, header.data(), sizeof(header), &read, nullptr));
    auto payload = std::string(header[1], '\0');
    REQUIRE(ReadFile(pipe, payload.data(), header[1], &read, nullptr));
    const auto reply   = encodeFrame(1, R"({"cmd":"DISPATCH","evt":"READY"})");
    auto       written = DWORD{};
    REQUIRE(WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr));
    return { header[0], nlohmann::json::parse(payload) };
}

} // namespace

TEST_CASE("Frames carry a little-endian opcode and length", "[discord]")
{
    const auto frame = encodeFrame(1, "{}");
    REQUIRE(frame == std::string("\x01\0\0\0\x02\0\0\0{}", 10));
}

TEST_CASE("Activity commands include only the fields that are set", "[discord]")
{
    const auto minimal = nlohmann::json::parse(activityCommand({ "Playing online" }, 42, 7));
    REQUIRE(minimal["cmd"] == "SET_ACTIVITY");
    REQUIRE(minimal["nonce"] == "7");
    REQUIRE(minimal["args"]["pid"] == 42);
    REQUIRE(minimal["args"]["activity"] == nlohmann::json{ { "details", "Playing online" } });

    const auto full     = nlohmann::json::parse(activityCommand({ "Playing online", "tm.example.com", 1700000000, "logo", "Tetra Master" }, 42, 8));
    const auto activity = full["args"]["activity"];
    REQUIRE(activity["state"] == "tm.example.com");
    REQUIRE(activity["timestamps"]["start"] == 1700000000);
    REQUIRE(activity["assets"]["large_image"] == "logo");
    REQUIRE(activity["assets"]["large_text"] == "Tetra Master");
}

TEST_CASE("Presence handshakes with the local client and sets the activity", "[discord]")
{
    const auto prefix = std::format(L"\\\\.\\pipe\\tmloader-test-discord-{}-", GetCurrentProcessId());
    const auto server = wil::unique_hfile(CreateNamedPipeW((prefix + L"0").c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
    REQUIRE(server);
    auto handshake = std::pair<std::uint32_t, nlohmann::json>{};
    auto activity  = std::pair<std::uint32_t, nlohmann::json>{};
    auto discord   = std::jthread([&]
                                  {
                                    ConnectNamedPipe(server.get(), nullptr);
                                    handshake = exchange(server.get());
                                    activity  = exchange(server.get());
                                  });

    auto presence = DiscordPresence("123456789", prefix);
    REQUIRE(presence.connected());
    presence.set({ "Playing on a local server" });
    discord.join();
    REQUIRE(presence.connected());
    REQUIRE(handshake.first == 0);
    REQUIRE(handshake.second == nlohmann::json{ { "v", 1 }, { "client_id", "123456789" } });
    REQUIRE(activity.first == 1);
    REQUIRE(activity.second["args"]["activity"]["details"] == "Playing on a local server");
}

TEST_CASE("Presence is inactive without an application or a running client", "[discord]")
{
    REQUIRE_FALSE(DiscordPresence("").connected());
    const auto missing = DiscordPresence("123456789", std::format(L"\\\\.\\pipe\\tmloader-test-missing-{}-", GetCurrentProcessId()));
    REQUIRE_FALSE(missing.connected());
}
