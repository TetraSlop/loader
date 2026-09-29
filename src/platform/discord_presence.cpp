#include "platform/discord_presence.h"
#include "app/log.h"
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>
#include <thread>

namespace tmloader
{
namespace
{

constexpr std::uint32_t handshakeOpcode = 0;
constexpr std::uint32_t frameOpcode     = 1;
constexpr auto          replyTimeout    = std::chrono::seconds(2);

} // namespace

auto encodeFrame(std::uint32_t opcode, std::string_view payload) -> std::string
{
    const auto length = static_cast<std::uint32_t>(payload.size());
    auto       frame  = std::string(8, '\0');
    std::memcpy(frame.data(), &opcode, sizeof(opcode));
    std::memcpy(frame.data() + 4, &length, sizeof(length));
    return frame.append(payload);
}

auto activityCommand(const Activity& activity, DWORD processId, int nonce) -> std::string
{
    auto details = nlohmann::json::object();
    if (!activity.details.empty())
    {
        details["details"] = activity.details;
    }

    if (!activity.state.empty())
    {
        details["state"] = activity.state;
    }

    if (activity.startedAt)
    {
        details["timestamps"] = { { "start", activity.startedAt } };
    }

    if (!activity.largeImage.empty())
    {
        details["assets"] = { { "large_image", activity.largeImage }, { "large_text", activity.largeText } };
    }

    return nlohmann::json{ { "cmd", "SET_ACTIVITY" }, { "args", { { "pid", processId }, { "activity", details } } }, { "nonce", std::to_string(nonce) } }.dump();
}

DiscordPresence::DiscordPresence(std::string_view applicationId, std::wstring_view pipePrefix)
{
    if (applicationId.empty())
    {
        return;
    }

    // Discord listens on the first free pipe of discord-ipc-0 through -9.
    for (int i = 0; i < 10 && !pipe_; ++i)
    {
        const auto name = std::format(L"{}{}", pipePrefix, i);
        pipe_.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    }

    const auto handshake = nlohmann::json{ { "v", 1 }, { "client_id", applicationId } }.dump();
    if (!pipe_ || !send(handshakeOpcode, handshake) || !receive())
    {
        pipe_.reset();
        return;
    }

    connected_ = true;
    worker_    = std::jthread([this](std::stop_token stop)
                              {
                               publish(stop);
                              });
    logLine("Discord Rich Presence connected");
}

auto DiscordPresence::connected() const noexcept -> bool
{
    return connected_;
}

void DiscordPresence::set(const Activity& activity)
{
    {
        const std::lock_guard lock(mutex_);
        pending_ = activity;
    }

    wake_.notify_one();
}

void DiscordPresence::publish(std::stop_token stop)
{
    auto nonce = 0;
    auto lock  = std::unique_lock(mutex_);
    while (wake_.wait(lock, stop, [this]
                      {
                          return pending_.has_value();
                      }))
    {
        const auto activity = *std::exchange(pending_, std::nullopt);
        lock.unlock();
        const auto sent = send(frameOpcode, activityCommand(activity, GetCurrentProcessId(), ++nonce)) && receive();
        lock.lock();
        if (!sent)
        {
            logLine("Discord Rich Presence disconnected");
            connected_ = false;
            return;
        }
    }
}

auto DiscordPresence::send(std::uint32_t opcode, const std::string& payload) -> bool
{
    const auto frame   = encodeFrame(opcode, payload);
    auto       written = DWORD{};
    return WriteFile(pipe_.get(), frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size();
}

// Consumes one reply so the pipe never fills; gives up if Discord doesn't answer.
auto DiscordPresence::receive() -> bool
{
    const auto deadline  = std::chrono::steady_clock::now() + replyTimeout;
    auto       available = DWORD{};
    while (PeekNamedPipe(pipe_.get(), nullptr, 0, nullptr, &available, nullptr) && available < 8)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto header = std::array<std::uint32_t, 2>{};
    auto read   = DWORD{};
    if (available < 8 || !ReadFile(pipe_.get(), header.data(), sizeof(header), &read, nullptr) || read != sizeof(header) || header[1] > 64 * 1024)
    {
        return false;
    }

    auto payload = std::string(header[1], '\0');
    return header[1] == 0 || (ReadFile(pipe_.get(), payload.data(), header[1], &read, nullptr) && read == header[1]);
}

} // namespace tmloader
