// The GL side of a D3D resource, which the device carries and objects.cpp fills on first use.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

struct __GLsync;

namespace gl
{

// 0 until a draw needs it, and always under Node.
struct Name
{
    unsigned id     = 0;
    unsigned target = 0; // a texture target, GL_RENDERBUFFER or a buffer target

    Name()                               = default;
    Name(const Name&)                    = delete;
    Name(Name&&)                         = delete;
    auto operator=(const Name&) -> Name& = delete;
    auto operator=(Name&&) -> Name&      = delete;
    ~Name();
};

// An occlusion query's copy on its way back from the GPU; the fence says when the pixels are in the buffer.
struct Readback
{
    unsigned  buffer = 0;
    __GLsync* fence  = nullptr;
    long      x      = 0;
    long      y      = 0;
    long      width  = 0;
    long      height = 0;

    Readback()                                   = default;
    Readback(const Readback&)                    = delete;
    Readback(Readback&&)                         = delete;
    auto operator=(const Readback&) -> Readback& = delete;
    auto operator=(Readback&&) -> Readback&      = delete;
    ~Readback();
};

// The bytes of a buffer written since its last upload; all of them at first. end is clamped to the buffer's size.
struct Dirty
{
    size_t begin = 0;
    size_t end   = SIZE_MAX;

    void Mark(size_t from, size_t to) noexcept
    {
        begin = std::min(begin, from);
        end   = std::max(end, to);
    }

    void Clear() noexcept
    {
        begin = SIZE_MAX;
        end   = 0;
    }
};

} // namespace gl
