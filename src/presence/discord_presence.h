#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <wil/resource.h>

namespace tmloader
{

struct Activity
{
    std::string  details;
    std::string  state;
    std::int64_t startedAt = 0; // Unix seconds; Discord shows the elapsed time.
    std::string  largeImage;    // Art asset key uploaded in the Discord developer portal.
    std::string  largeText;
};

// Discord Rich Presence over the desktop client's local IPC pipe. Silent no-op
// when Discord isn't running. Connect before the process lowers its integrity:
// a low-integrity process cannot open the pipe, but an already open handle keeps working.
// set() only records the latest activity; a worker thread sends it.
class DiscordPresence
{
public:
    explicit DiscordPresence(std::string_view applicationId, std::wstring_view pipePrefix = L"\\\\.\\pipe\\discord-ipc-");

    auto connected() const noexcept -> bool;
    void set(const Activity& activity);

private:
    auto send(std::uint32_t opcode, const std::string& payload) -> bool;
    auto receive() -> bool;
    void publish(const std::stop_token& stop);

    wil::unique_hfile           pipe_;
    std::atomic<bool>           connected_{ false };
    std::mutex                  mutex_;
    std::condition_variable_any wake_;
    std::optional<Activity>     pending_;
    std::jthread                worker_;
};

// One IPC frame: little-endian opcode and length, then the JSON payload.
auto encodeFrame(std::uint32_t opcode, std::string_view payload) -> std::string;
auto activityCommand(const Activity& activity, DWORD processId, int nonce) -> std::string;

} // namespace tmloader
