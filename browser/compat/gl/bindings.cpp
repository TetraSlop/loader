// GL bindings as last made, so draws skip the calls that would change nothing: each one leaves Wasm for WebGL's checks.
#include "internal.h"

namespace gl
{

namespace
{

constexpr GLuint kUnknown = ~0u;

struct Pointer
{
    GLuint    buffer;
    GLint     size;
    GLenum    type;
    GLboolean normalized;
    GLsizei   stride;
    size_t    offset;

    auto operator==(const Pointer& other) const noexcept -> bool
    {
        return buffer == other.buffer && size == other.size && type == other.type && normalized == other.normalized && stride == other.stride && offset == other.offset;
    }
};

struct AttributeState
{
    bool    known   = false; // enabled and pointer
    bool    enabled = false;
    Pointer pointer{};
    int     value = -1; // when disabled: -1 unknown, 0 for (0, 0, 0, 1), 1 for (1, 1, 1, 1)
};

struct Bindings
{
    GLuint                                                    unit = kUnknown;
    std::array<std::array<GLuint, 2>, nulldx::kTextureStages> textures; // 2D, cube
    std::array<GLuint, nulldx::kTextureStages>                samplers;
    std::array<GLuint, 2>                                     buffers; // array, element array
    GLuint                                                    drawFramebuffer = kUnknown;
    std::array<AttributeState, kAttributes>                   attributes;

    Bindings()
    {
        for (auto& unit : textures)
        {
            unit.fill(kUnknown);
        }

        samplers.fill(kUnknown);
        buffers.fill(kUnknown);
    }
};

Bindings g_bound;

} // namespace

void ForgetBindings()
{
    g_bound = Bindings();
}

void BindBuffer(GLenum target, GLuint id)
{
    auto& bound = g_bound.buffers[target == GL_ELEMENT_ARRAY_BUFFER];
    if (bound != id)
    {
        bound = id;
        glBindBuffer(target, id);
    }
}

void BindTexture(GLuint unit, GLenum target, GLuint id)
{
    auto& bound = g_bound.textures[unit][target == GL_TEXTURE_CUBE_MAP];
    if (bound == id)
    {
        return;
    }

    if (g_bound.unit != unit)
    {
        g_bound.unit = unit;
        glActiveTexture(GL_TEXTURE0 + unit);
    }

    bound = id;
    glBindTexture(target, id);
}

void BindTexture(GLenum target, GLuint id)
{
    const auto unit = [&]() -> GLuint
    {
        if (g_bound.unit == kUnknown)
        {
            return 0;
        }

        return g_bound.unit;
    }();

    BindTexture(unit, target, id);
}

void BindSampler(GLuint unit, GLuint sampler)
{
    auto& bound = g_bound.samplers[unit];
    if (bound != sampler)
    {
        bound = sampler;
        glBindSampler(unit, sampler);
    }
}

void BindDrawFramebuffer(GLuint id)
{
    if (g_bound.drawFramebuffer != id)
    {
        g_bound.drawFramebuffer = id;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, id);
    }
}

void AttributePointer(GLuint location, GLuint buffer, const Element& element, GLsizei stride, size_t offset)
{
    const auto pointer = [&]() -> Pointer
    {
        if (element.color)
        {
            return Pointer{ buffer, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, offset };
        }

        return Pointer{ buffer, static_cast<GLint>(element.components), GL_FLOAT, GL_FALSE, stride, offset };
    }();

    auto& state = g_bound.attributes[location];
    if (!state.known || !state.enabled)
    {
        glEnableVertexAttribArray(location);
    }

    if (!state.known || !state.enabled || !(state.pointer == pointer))
    {
        BindBuffer(GL_ARRAY_BUFFER, buffer);
        glVertexAttribPointer(location, pointer.size, pointer.type, pointer.normalized, pointer.stride, reinterpret_cast<const void*>(pointer.offset));
    }

    state.known   = true;
    state.enabled = true;
    state.pointer = pointer;
}

// D3D reads a missing diffuse colour as white; GL's default attribute is (0, 0, 0, 1).
void AttributeDefault(GLuint location)
{
    const auto white = static_cast<int>(location == kDiffuse);
    auto&      state = g_bound.attributes[location];
    if (!state.known || state.enabled)
    {
        glDisableVertexAttribArray(location);
    }

    state.known   = true;
    state.enabled = false;
    if (state.value == white)
    {
        return;
    }

    state.value    = white;
    const auto rgb = [&]() -> float
    {
        if (white)
        {
            return 1.0f;
        }

        return 0.0f;
    }();

    glVertexAttrib4f(location, rgb, rgb, rgb, 1.0f);
}

} // namespace gl
