#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
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
class DiscordPresence
{
public:
    explicit DiscordPresence(std::string_view applicationId, std::wstring_view pipePrefix = L"\\\\.\\pipe\\discord-ipc-");

    auto connected() const noexcept -> bool;
    void set(const Activity& activity) noexcept;

private:
    auto send(std::uint32_t opcode, const std::string& payload) -> bool;
    auto receive() -> bool;

    wil::unique_hfile pipe_;
};

// One IPC frame: little-endian opcode and length, then the JSON payload.
auto encodeFrame(std::uint32_t opcode, std::string_view payload) -> std::string;
auto activityCommand(const Activity& activity, DWORD processId, int nonce) -> std::string;

} // namespace tmloader
