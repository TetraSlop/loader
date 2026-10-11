#include "presence/game_phase.h"
#include "app/log.h"
#include "app/runtime.h"
#include "platform/patch.h"
#include "presence/presence.h"
#include <charconv>
#include <cstring>
#include <optional>

namespace tmloader
{
namespace
{

// The message pump's call to the game's GTM0 ingress check, which receives one
// queued message record; the message text starts at +0x1D of a 0x220-byte record.
constexpr DWORD kIngressCall   = 0x19e57f;
constexpr DWORD kIngressTarget = 0x1a2b90;
constexpr DWORD kMessageText   = 0x1d;
constexpr DWORD kRecordSize    = 0x220;

constexpr unsigned kMatchFamily = 0x43;

using IngressFn = int(__cdecl*)(const BYTE* record);

IngressFn                      originalIngress = nullptr;
GamePhase                      currentPhase    = GamePhase::menus;
std::function<void(GamePhase)> listener;

// Two hex digits at offset, as in the GTM0G header.
auto hexByte(std::string_view message, size_t offset) -> std::optional<unsigned>
{
    auto value = 0U;
    if (message.size() < offset + 2 ||
        std::from_chars(message.data() + offset, message.data() + offset + 2, value, 16).ptr != message.data() + offset + 2)
    {
        return std::nullopt;
    }

    return value;
}

auto __cdecl hookedIngress(const BYTE* record) -> int
{
    const auto accepted = originalIngress(record);
    if (accepted != 1)
    {
        return accepted;
    }

    const auto* const text    = reinterpret_cast<const char*>(record + kMessageText);
    const auto        message = std::string_view(text, strnlen(text, kRecordSize - kMessageText));
    if (options.diagnostics)
    {
        // Header only; bodies can contain player data.
        logLine("Game message {}", message.substr(0, 13));
    }

    const auto next = nextPhase(currentPhase, message);
    if (next != currentPhase)
    {
        currentPhase = next;
        logLine("Game phase: {}", describePhase(next));
        if (listener)
        {
            listener(next);
        }
    }

    return accepted;
}

} // namespace

auto nextPhase(GamePhase current, std::string_view message) -> GamePhase
{
    if (!message.starts_with("GTM0") || message.size() < 5)
    {
        return current;
    }

    // Room-service traffic means the player reached the lobby.
    if (message[4] == 'L')
    {
        if (current == GamePhase::menus)
        {
            return GamePhase::lobby;
        }

        return current;
    }

    // GTM0G, family, reserved, subtype, argument: two hex digits each.
    const auto family   = hexByte(message, 5);
    const auto subtype  = hexByte(message, 9);
    const auto argument = hexByte(message, 11);
    if (message[4] != 'G' || family != kMatchFamily || !subtype)
    {
        return current;
    }

    switch (*subtype)
    {
        case 0x01: // VsGameInit / EventGameInit
        case 0x02: // ComGameInit
        case 0x08: // StartData
        case 0x09: // TurnData
        case 0x0A: // PutCard
        case 0x0C: // BattleData
            return GamePhase::match;
        case 0x0D: // ResultData
            return GamePhase::results;
        case 0x11: // Continue reply: 1 rematch, 2 end
            if (argument == 1U)
            {
                return GamePhase::match;
            }

            return GamePhase::lobby;
        case 0x05: // Quit reply
            return GamePhase::lobby;
        case 0x25: // WatchInfo
        case 0x26: // WatchCard
            return GamePhase::watching;
        default:
            return current;
    }
}

auto describePhase(GamePhase phase) -> const char*
{
    switch (phase)
    {
        case GamePhase::menus:
            return "In the menus";
        case GamePhase::lobby:
            return "In the lobby";
        case GamePhase::match:
            return "In a match";
        case GamePhase::results:
            return "Viewing results";
        case GamePhase::watching:
            return "Watching a match";
    }

    return "";
}

void setGamePhaseListener(std::function<void(GamePhase)> value)
{
    listener = std::move(value);
}

void installPresenceHooks(HMODULE game)
{
    try
    {
        originalIngress = reinterpret_cast<IngressFn>(redirectCall(game, kIngressCall, kIngressTarget, reinterpret_cast<void*>(hookedIngress)));
    }
    catch (const std::exception& error)
    {
        logLine("Rich Presence will not follow the game: {}", error.what());
    }
}

} // namespace tmloader
