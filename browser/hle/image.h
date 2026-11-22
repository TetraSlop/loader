#pragma once

// PE images in guest memory: map a DLL file, bind its header imports to stand-ins, look up exports.

#include "core/boundary.h"
#include "core/memory.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace tmweb::hle
{

struct Image
{
    std::uint32_t base  = 0;
    std::uint32_t size  = 0;
    std::uint32_t entry = 0; // 0 when the image has no entry point
};

// Map headers and sections and point every header import at boundary.resolve(). At the preferred base (TM.dll's is
// pinned) nothing is relocated; at another base the data directory's relocations are applied, as the OS loader does,
// so polcore's POL1 stub still finds its own hidden .text relocations unapplied. Throws std::runtime_error for a file
// that is not a 32-bit PE.
auto loadImage(Memory& memory, Boundary& boundary, std::span<const std::uint8_t> file, std::uint32_t base = 0) -> Image;

// The address an image in guest memory exports under name ("#n" for an ordinal), or 0
auto findExport(const Memory& memory, std::uint32_t base, std::string_view name) -> std::uint32_t;

} // namespace tmweb::hle
