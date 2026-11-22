#pragma once

// Host stand-ins for D3DX functions TM.dll links statically, where running the guest's own copy costs most of a
// frame (see d3dx.cpp).

#include "core/boundary.h"

#include <string>
#include <vector>

namespace tmweb
{

class Proxies;

// Adds the stand-ins to the DirectX exports addDirectX returns. They are patched over TM.dll's code the first time
// Direct3DCreate8 runs, when the protector has long unpacked it.
void addD3dx(Proxies& proxies, std::vector<Function>& functions);

// Verification: each call also runs the game's own D3DX and compares the texels. report() lists, per file format and
// channel count, how many textures matched byte for byte and how many did not.
void verifyD3dx(bool on);
auto d3dxReport() -> std::string;

} // namespace tmweb
