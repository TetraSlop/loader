#pragma once

// d3d8.dll, dinput8.dll and dsound.dll for the guest, answered by compat's Direct3D 8 on WebGL2, DirectInput and
// DirectSound through COM proxies. Wasm only: compat builds against the Windows headers.

#include "core/boundary.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace tmweb
{

class Proxies;

struct Pointer
{
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// What the stand-ins need from the host; each part is optional
struct MediaHost
{
    // After each Present, once the frame is on the canvas
    std::function<void()> presented;
    // The DirectSound buffers mixed as the machine's clock runs: interleaved stereo float frames at soundRate. Without
    // it they play on unheard.
    std::function<void(std::span<const float>)> sound;
    unsigned                                    soundRate = 48000;
    // The host's pointer in back-buffer pixels, once it has one
    std::function<std::optional<Pointer>()> pointer;
    // The guest address of TM's software cursor block (image + 0x2FF570). With pointer, mouse deltas steer the
    // game's cursor onto the host's; 0 passes DirectInput's deltas through.
    std::uint32_t gameCursor = 0;
};

// Registers every interface the three DLLs hand out with proxies, and returns their exports for the boundary:
// Direct3DCreate8, DirectInput8Create, and DirectSoundCreate8 by name and as ordinal 11.
auto addDirectX(Proxies& proxies, MediaHost host = {}) -> std::vector<Function>;

} // namespace tmweb
