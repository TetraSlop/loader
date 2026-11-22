#include "host/host.h"

#include "app/boot.h"
#include "core/machine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <utility>

#ifdef __EMSCRIPTEN__
// web/host.js
extern "C"
{
    void tmweb_host_log(const char* text, int length);
    void tmweb_host_connect(int id, const char* address, int port);
    void tmweb_host_send(int id, const std::uint8_t* bytes, int length);
    void tmweb_host_close(int id);
}
#else
namespace
{

void tmweb_host_log(const char* text, const int length)
{
    std::fprintf(stderr, "%.*s\n", length, text);
}

// No transport: every connect fails, as an unrouted one does on the page
void tmweb_host_connect(const int id, const char*, int)
{
    tmweb_socket_event(id, 3, nullptr, 0);
}

void tmweb_host_send(int, const std::uint8_t*, int)
{
}

void tmweb_host_close(int)
{
}

} // namespace
#endif

namespace tmweb::host
{
namespace
{

std::vector<InputEvent>                        g_input;
std::vector<float>                             g_sound;
std::vector<float>                             g_soundTaken;
std::map<std::uint32_t, std::weak_ptr<Socket>> g_sockets;
std::uint32_t                                  g_nextSocket = 1;

} // namespace

void log(const std::string_view line)
{
    tmweb_host_log(line.data(), static_cast<int>(line.size()));
}

auto takeInput() -> std::vector<InputEvent>
{
    return std::exchange(g_input, {});
}

void playSound(const std::span<const float> frames)
{
    g_sound.insert(g_sound.end(), frames.begin(), frames.end());
}

Socket::Socket(const std::uint32_t id)
: id_(id)
{
}

auto Socket::id() const -> std::uint32_t
{
    return id_;
}

auto Socket::state() const -> SocketState
{
    return state_;
}

auto Socket::available() const -> std::size_t
{
    return inbox_.size();
}

auto Socket::receive(const std::span<std::uint8_t> into) -> std::size_t
{
    const auto count = std::min(into.size(), inbox_.size());
    std::copy_n(inbox_.begin(), count, into.begin());
    inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

void Socket::send(const std::span<const std::uint8_t> bytes)
{
    if (state_ == SocketState::Open)
    {
        tmweb_host_send(static_cast<int>(id_), bytes.data(), static_cast<int>(bytes.size()));
    }
}

void Socket::close()
{
    if (state_ == SocketState::Connecting || state_ == SocketState::Open)
    {
        state_ = SocketState::Closed;
        tmweb_host_close(static_cast<int>(id_));
    }
}

void Socket::opened()
{
    if (state_ == SocketState::Connecting)
    {
        state_ = SocketState::Open;
    }
}

void Socket::received(const std::span<const std::uint8_t> bytes)
{
    inbox_.insert(inbox_.end(), bytes.begin(), bytes.end());
}

void Socket::ended(const SocketState state)
{
    if (state_ == SocketState::Connecting || state_ == SocketState::Open)
    {
        state_ = state;
    }
}

auto connect(const std::string_view address, const std::uint16_t port) -> std::shared_ptr<Socket>
{
    const auto socket       = std::make_shared<Socket>(g_nextSocket++);
    g_sockets[socket->id()] = socket;
    tmweb_host_connect(static_cast<int>(socket->id()), std::string{ address }.c_str(), port);
    return socket;
}

} // namespace tmweb::host

namespace
{

std::unique_ptr<tmweb::Machine> g_machine;
std::uint64_t                   g_instructions = 0;
bool                            g_halted       = false;

} // namespace

extern "C" auto tmweb_boot(const char* root, const int trace, const char* until) -> int
{
    using tmweb::host::log;
    auto error = std::error_code{};
    if (!std::filesystem::is_directory(root, error))
    {
        log(std::format("tmweb: no runtime folder at {}", root));
        return 1;
    }
    g_machine       = std::make_unique<tmweb::Machine>();
    g_instructions  = 0;
    g_halted        = false;
    const auto game = std::filesystem::path{ root } / "TM_native.dll";
    log(std::format("tmweb: runtime {}, TM_native.dll {} bytes, guest memory base {:#x}", root, std::filesystem::exists(game, error) ? std::filesystem::file_size(game, error) : 0, g_machine->memory().base()));
    if (trace != 0 || *until != 0)
    {
        g_machine->boundary().observe(
            [trace, until = std::string{ until }](const tmweb::Call& call)
            {
                if (trace != 0)
                {
                    log(std::format("call {} from {:#x}", call.name(), call.returnAddress()));
                }
                if (call.name() == until)
                {
                    call.machine().halt(std::format("reached {}", until));
                }
            });
    }
    tmweb::boot(*g_machine, root);
    return 0;
}

extern "C" auto tmweb_run(const double budget) -> int
{
    if (!g_machine || g_halted)
    {
        return 2;
    }
    // Machine::run takes a budget for this call and reports what it retired
    const auto result = g_machine->run(static_cast<std::uint64_t>(budget));
    g_instructions += result.instructions;
    if (result.halted)
    {
        g_halted = true;
        tmweb::host::log(std::format("tmweb: halted after {} instructions: {}", g_instructions, result.reason));
        return 2;
    }
    return result.idle ? 1 : 0;
}

extern "C" auto tmweb_instructions() -> double
{
    return static_cast<double>(g_instructions);
}

extern "C" void tmweb_input(const int kind, const int a, const int b)
{
    tmweb::host::g_input.push_back({ static_cast<tmweb::host::InputKind>(kind), a, b });
}

extern "C" auto tmweb_sound_take() -> const float*
{
    using namespace tmweb::host;
    g_soundTaken = std::exchange(g_sound, {});
    return g_soundTaken.data();
}

extern "C" auto tmweb_sound_frames() -> int
{
    return static_cast<int>(tmweb::host::g_soundTaken.size() / 2);
}

extern "C" void tmweb_socket_event(const int id, const int kind, const std::uint8_t* bytes, const int length)
{
    using namespace tmweb::host;
    const auto found = g_sockets.find(static_cast<std::uint32_t>(id));
    if (found == g_sockets.end())
    {
        return;
    }
    const auto socket = found->second.lock();
    if (!socket)
    {
        g_sockets.erase(found);
        return;
    }
    switch (kind)
    {
        case 0:
            socket->opened();
            break;
        case 1:
            socket->received({ bytes, static_cast<std::size_t>(length) });
            break;
        case 2:
            socket->ended(SocketState::Closed);
            break;
        default:
            socket->ended(SocketState::Failed);
            break;
    }
}
