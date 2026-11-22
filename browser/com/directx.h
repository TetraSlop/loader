#pragma once

// d3d8.dll, dinput8.dll and dsound.dll for the guest, answered by compat's Direct3D 8 on WebGL2, DirectInput and
// DirectSound through COM proxies. Wasm only: compat builds against the Windows headers.

#include "core/boundary.h"

#include <vector>

namespace tmweb
{

class Proxies;

// Registers every interface the three DLLs hand out with proxies, and returns their exports for the boundary:
// Direct3DCreate8, DirectInput8Create, and DirectSoundCreate8 by name and as ordinal 11.
auto addDirectX(Proxies& proxies) -> std::vector<Function>;

} // namespace tmweb
