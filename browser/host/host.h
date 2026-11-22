#pragma once

// The module's way out to its host: the page's worker or the Node runner (browser/web). Stand-ins reach the outside
// world only through here: the log, the keyboard and mouse, frames, file writes, the speakers and stream sockets. Under Emscripten the
// JS side (web/host.js) implements the outward calls; natively the log goes to stderr and every connect fails.
//
// The game files are plain files under the runtime root that tmweb_boot gets: the picked folder copied into MEMFS on
// the page, the real folder mounted with NODEFS under Node. Read them with the C++ standard library; after writing
// one, call fileWritten so the page keeps it (OPFS).
//
// Direct3D draws on the canvas the host hands the module factory (Module.canvas, an OffscreenCanvas in the worker),
// as Kestrel's compat gl/gl.js expects. Under Node there is none.

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tmweb::host
{

// One line of the module's log
void log(std::string_view line);

enum class InputKind : std::uint8_t
{
    Key,    // a: DirectInput key code (DIK_*), b: 1 down, 0 up
    Move,   // a, b: the pointer in client pixels, 0-639 and 0-479
    Button, // a: 0 left, 1 right, 2 middle; b: 1 down, 0 up
    Wheel,  // a: 120ths of a notch, away from the user positive
};

struct InputEvent
{
    InputKind    kind = InputKind::Key;
    std::int32_t a    = 0;
    std::int32_t b    = 0;
};

// The keyboard and mouse as the host's events left them
struct InputState
{
    std::array<bool, 256> keys{};  // by DirectInput key code (DIK_*), as InputEvent's key
    std::int32_t          x = 320; // the pointer in client pixels
    std::int32_t          y = 240;
    std::array<bool, 3>   buttons{}; // 0 left, 1 right, 2 middle
};

auto inputState() -> const InputState&;
// Called with each event as it arrives, between run slices, after inputState() reflects it. Under Emscripten the
// events also reach compat's DirectInput (kr_dinput_key, kr_dinput_mouse with the pointer's motion, kr_dinput_button).
void onInput(std::function<void(const InputEvent&)> listener);

// A frame went to the canvas: Present calls this after gl::Present. It counts frames (tmweb_frames) and ends the run
// slice, so each of the host's tasks shows one frame.
void presented();

// The guest wrote path (relative to the runtime root, '/'-separated) to the files under the root; the page copies it
// to OPFS. Under Node and natively it does nothing, the folder being the real one.
void fileWritten(std::string_view path);

// Sound: interleaved stereo float frames at kSoundRate for the page's speakers. Under Node it is dropped.
inline constexpr unsigned kSoundRate = 48000;
void                      playSound(std::span<const float> frames);

enum class SocketState : std::uint8_t
{
    Connecting,
    Open,
    Closed, // the peer closed, or close() was called
    Failed, // no route to that address, or the transport dropped
};

// A stream socket over the host's transport, one WebSocket each. The host maps address:port to a URL; until the
// gateway's browser sign-in lands, no address has one and every connect fails. A guest that must wait for state or
// data blocks on a Waitable that checks it (core/threads.h): the host's events arrive between run slices.
class Socket final
{
public:
    explicit Socket(std::uint32_t id);

    auto id() const -> std::uint32_t;
    auto state() const -> SocketState;
    auto available() const -> std::size_t;
    // Up to into.size() received bytes; 0 when none have arrived
    auto receive(std::span<std::uint8_t> into) -> std::size_t;
    void send(std::span<const std::uint8_t> bytes);
    void close();

    // Transport events (tmweb_socket_event)
    void opened();
    void received(std::span<const std::uint8_t> bytes);
    void ended(SocketState state);

private:
    std::uint32_t            id_;
    SocketState              state_ = SocketState::Connecting;
    std::deque<std::uint8_t> inbox_;
};

// Open a stream to address (dotted IPv4 or a host name) and port; it starts Connecting
auto connect(std::string_view address, std::uint16_t port) -> std::shared_ptr<Socket>;

} // namespace tmweb::host

// What the host calls, exported from the module (web/module.js)
extern "C"
{
    // Before tmweb_boot: one boot option. "viewer" "polcore" runs the user's polcore_native.dll instead of the
    // stand-in Viewer; "probe" "1" then logs in to Game IRC and loads the profile instead of booting TM.dll;
    // "keygen" "0" runs the RSA key generation on polcore's own code; "pol_id" and "password" are the play
    // session's legacy account. 0 on success, 1 for an unknown key or value.
    auto tmweb_configure(const char* key, const char* value) -> int;
    // Make the machine and boot the runtime folder at root. trace logs every stand-in call; until (empty for none)
    // halts at the first call to that "module!name". 0 on success.
    auto tmweb_boot(const char* root, int trace, const char* until) -> int;
    // Run up to budget instructions. 0: ran the budget out, 1: idle until a host event or timeout, 2: halted (the
    // reason is logged), 3: a frame was presented.
    auto tmweb_run(double budget) -> int;
    // Frames presented since boot
    auto tmweb_frames() -> double;
    // Instructions retired since boot
    auto tmweb_instructions() -> double;
    void tmweb_input(int kind, int a, int b);
    // The sound frames played since the last take: tmweb_sound_take gives the frames' address, then
    // tmweb_sound_frames their count, valid until the next take
    auto tmweb_sound_take() -> const float*;
    auto tmweb_sound_frames() -> int;
    // kind 0: opened, 1: bytes arrived, 2: closed, 3: failed
    void tmweb_socket_event(int id, int kind, const std::uint8_t* bytes, int length);
}
