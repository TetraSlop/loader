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
    void tmweb_host_file_written(const char* path, int length);
    // compat/dx/dinput.cpp
    void kr_dinput_key(int key, bool down);
    void kr_dinput_mouse(int dx, int dy, int dz);
    void kr_dinput_button(int button, bool down);
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

void tmweb_host_file_written(const char*, int)
{
}

} // namespace
#endif

namespace tmweb::host
{
namespace
{

InputState                                          g_inputState;
std::vector<std::function<void(const InputEvent&)>> g_inputListeners;
std::uint64_t                                       g_frames    = 0;
bool                                                g_presented = false;
tmweb::Machine*                                     g_running   = nullptr; // what presented() yields
std::vector<float>                                  g_sound;
std::vector<float>                                  g_soundTaken;
std::map<std::uint32_t, std::weak_ptr<Socket>>      g_sockets;
std::uint32_t                                       g_nextSocket = 1;

} // namespace

void log(const std::string_view line)
{
    tmweb_host_log(line.data(), static_cast<int>(line.size()));
}

auto inputState() -> const InputState&
{
    return g_inputState;
}

void onInput(std::function<void(const InputEvent&)> listener)
{
    g_inputListeners.push_back(std::move(listener));
}

void presented()
{
    ++g_frames;
    g_presented = true;
    // End the run at this frame, so each host task shows one
    if (g_running)
    {
        g_running->yield();
    }
}

void fileWritten(const std::string_view path)
{
    tmweb_host_file_written(path.data(), static_cast<int>(path.size()));
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
tmweb::hle::BootOptions         g_options;
std::uint64_t                   g_instructions = 0;
bool                            g_halted       = false;

} // namespace

// One --trace line: the stand-in, its caller, the thread, the first four stack arguments, and any argument that
// reads as a short printable string (a file name, a registry value, a window class)
auto traceLine(const tmweb::Call& call) -> std::string
{
    const auto& memory = call.memory();
    const auto  esp    = call.context().r[tmweb::kEsp];
    auto        line   = std::format("call {} from {:#x} thread {}", call.name(), memory.mapped(esp, 4) ? call.returnAddress() : 0, call.machine().scheduler().current().id);
    if (!memory.mapped(esp + 4, 16))
    {
        return line;
    }
    if (call.name() == "user32.dll!RegisterClassExA" && memory.mapped(call.arg(0), 48))
    {
        // WNDCLASSEXA: the window procedure at +8, the class name at +40
        const auto name      = memory.read<std::uint32_t>(call.arg(0) + 40);
        auto       className = std::string{};
        if (memory.mapped(name))
        {
            className = memory.readString(name, 128);
        }
        line += std::format(" wndproc {:#x} class \"{}\"", memory.read<std::uint32_t>(call.arg(0) + 8), className);
    }
    line += " args";
    for (auto index = 0u; index < 4; ++index)
    {
        line += std::format(" {:#x}", call.arg(index));
    }
    for (auto index = 0u; index < 4; ++index)
    {
        const auto address = call.arg(index);
        auto       text    = std::string{};
        for (auto offset = 0u; offset < 128 && memory.mapped(address + offset); ++offset)
        {
            const auto c = memory.read<char>(address + offset);
            if (c == '\0')
            {
                if (text.size() >= 2)
                {
                    line += std::format(" \"{}\"", text);
                }
                break;
            }
            if (c < 0x20 || c > 0x7E)
            {
                break;
            }
            text.push_back(c);
        }
    }
    return line;
}

extern "C" auto tmweb_configure(const char* key, const char* value) -> int
{
    const auto name = std::string_view{ key }, text = std::string_view{ value };
    if (name == "viewer" && (text == "polcore" || text == "stub"))
    {
        g_options.realViewer = text == "polcore";
    }
    else if (name == "probe" || name == "keygen")
    {
        (name == "probe" ? g_options.probe : g_options.keygenStandIns) = text == "1";
    }
    else if (name == "pol_id")
    {
        g_options.login.polId = text;
    }
    else if (name == "password")
    {
        g_options.login.password = text;
    }
    else
    {
        return 1;
    }
    return 0;
}

extern "C" auto tmweb_boot(const char* root, const int trace, const char* until) -> int
{
    using tmweb::host::log;
    auto error = std::error_code{};
    if (!std::filesystem::is_directory(root, error))
    {
        log(std::format("tmweb: no runtime folder at {}", root));
        return 1;
    }
    g_machine              = std::make_unique<tmweb::Machine>();
    tmweb::host::g_running = g_machine.get();
    g_instructions         = 0;
    g_halted               = false;
    const auto game        = std::filesystem::path{ root } / "TM_native.dll";
    log(std::format("tmweb: runtime {}, TM_native.dll {} bytes, guest memory base {:#x}", root, std::filesystem::exists(game, error) ? std::filesystem::file_size(game, error) : 0, g_machine->memory().base()));
    if (trace != 0 || *until != 0)
    {
        g_machine->boundary().observe(
            [trace, until = std::string{ until }](const tmweb::Call& call)
            {
                if (trace != 0)
                {
                    log(traceLine(call));
                }
                if (call.name() == until)
                {
                    call.machine().halt(std::format("reached {}", until));
                }
            });
    }
    tmweb::host::g_inputState = {};
    tmweb::host::g_inputListeners.clear();
    tmweb::host::g_frames = 0;
    tmweb::boot(*g_machine, root, g_options);
    return 0;
}

extern "C" auto tmweb_run(const double budget) -> int
{
    if (!g_machine || g_halted)
    {
        return 2;
    }
    // Machine::run takes a budget for this call and reports what it retired
    tmweb::host::g_presented = false;
    const auto result        = g_machine->run(static_cast<std::uint64_t>(budget));
    g_instructions += result.instructions;
    if (result.halted)
    {
        g_halted = true;
        tmweb::host::log(std::format("tmweb: halted after {} instructions: {}", g_instructions, result.reason));
        return 2;
    }
    if (tmweb::host::g_presented)
    {
        return 3;
    }
    return result.idle ? 1 : 0;
}

extern "C" auto tmweb_frames() -> double
{
    return static_cast<double>(tmweb::host::g_frames);
}

extern "C" auto tmweb_instructions() -> double
{
    return static_cast<double>(g_instructions);
}

extern "C" void tmweb_input(const int kind, const int a, const int b)
{
    using namespace tmweb::host;
    auto  event = InputEvent{ static_cast<InputKind>(kind), a, b };
    auto& state = g_inputState;
    switch (event.kind)
    {
        case InputKind::Key:
            if (a <= 0 || a > 0xFF)
            {
                return;
            }
            state.keys[static_cast<std::size_t>(a)] = b != 0;
#ifdef __EMSCRIPTEN__
            kr_dinput_key(a, b != 0);
#endif
            break;
        case InputKind::Move:
        {
            const auto x = std::clamp(a, 0, 639);
            const auto y = std::clamp(b, 0, 479);
#ifdef __EMSCRIPTEN__
            kr_dinput_mouse(x - state.x, y - state.y, 0);
#endif
            state.x = event.a = x;
            state.y = event.b = y;
            break;
        }
        case InputKind::Button:
            if (a < 0 || a > 2)
            {
                return;
            }
            state.buttons[static_cast<std::size_t>(a)] = b != 0;
#ifdef __EMSCRIPTEN__
            kr_dinput_button(a, b != 0);
#endif
            break;
        case InputKind::Wheel:
#ifdef __EMSCRIPTEN__
            kr_dinput_mouse(0, 0, a);
#endif
            break;
        default:
            return;
    }
    for (const auto& listener : g_inputListeners)
    {
        listener(event);
    }
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
