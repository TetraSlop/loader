#pragma once

#include "gl.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace gl
{

using nulldx::NullDevice;
using nulldx::NullSurface;

extern bool g_active; // a context exists
extern bool g_s3tc;   // WEBGL_compressed_texture_s3tc

auto FvfFor(const std::vector<Element>& layout) -> DWORD;

// 0 for none or a volume texture.
auto TextureFor(IDirect3DBaseTexture8* texture, GLenum& target) -> GLuint;
auto SamplerFor(const std::array<DWORD, nulldx::kTextureStageStates>& stage) -> GLuint;
auto BufferFor(nulldx::NullVertexBuffer* buffer) -> GLuint;
auto BufferFor(nulldx::NullIndexBuffer* buffer) -> GLuint;
// False when the device's render target cannot be drawn into.
auto BindTarget(const NullDevice& device) -> bool;
// 0 for a surface in CPU memory.
auto ReadFramebuffer(NullSurface* surface) -> GLuint;

// Each skips the call when it would change nothing. BindTexture without a unit binds on the active one.
void BindBuffer(GLenum target, GLuint id);
void BindTexture(GLuint unit, GLenum target, GLuint id);
void BindTexture(GLenum target, GLuint id);
void BindSampler(GLuint unit, GLuint sampler);
void BindDrawFramebuffer(GLuint id);
void AttributePointer(GLuint location, GLuint buffer, const Element& element, GLsizei stride, size_t offset);
void AttributeDefault(GLuint location);
// Deleting an object unbinds it wherever it was bound.
void ForgetBindings();

// A program keeps its uniforms, and most draws change few.
struct Uniform
{
    GLint                      location = -1;
    std::vector<unsigned char> value;

    // Never true for a uniform the program lacks.
    auto Changed(const void* data, size_t size) -> bool
    {
        const auto* const bytes = static_cast<const unsigned char*>(data);
        if (location < 0 || (value.size() == size && std::equal(value.begin(), value.end(), bytes)))
        {
            return false;
        }

        value.assign(bytes, bytes + size);
        return true;
    }

    template <class T>
    auto Changed(const T& data) -> bool
    {
        return Changed(&data, sizeof(T));
    }
};

struct Layout
{
    std::vector<Element> elements;
    DWORD                fvf; // for the fixed-function key
};

// Binds the draw's program with its uniforms set and returns the layout it reads; nullptr when the draw cannot run.
auto UseProgram(const NullDevice& device) -> const Layout*;

} // namespace gl
