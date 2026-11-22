// The device's draws, clears and presents in WebGL2.
#include "internal.h"

#include <emscripten/html5.h>
#include <webgl/webgl2.h>

#include <algorithm>
#include <cstring>

// gl.js: 0 when the host handed the module no canvas.
extern "C" auto kr_gl_context(int width, int height) -> int;

namespace gl
{

bool g_active;
bool g_s3tc;
bool g_drawing = true; // kr_gl_draw

namespace
{

using nulldx::NullIndexBuffer;
using nulldx::NullVertexBuffer;

struct Applied
{
    std::array<DWORD, nulldx::kRenderStates> renderStates;
    D3DVIEWPORT8                             viewport;
};

Applied g_applied;
bool    g_appliedValid;

GLuint g_upVertices;

auto Compare(DWORD func) noexcept -> GLenum
{
    if (func >= D3DCMP_NEVER && func <= D3DCMP_ALWAYS)
    {
        return GL_NEVER + func - 1;
    }

    return GL_ALWAYS;
}

auto Blend(DWORD blend) noexcept -> GLenum
{
    switch (blend)
    {
        case D3DBLEND_ZERO:
            return GL_ZERO;
        case D3DBLEND_SRCCOLOR:
            return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR:
            return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA:
            return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA:
            return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA:
            return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA:
            return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR:
            return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR:
            return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT:
            return GL_SRC_ALPHA_SATURATE;
        default:
            return GL_ONE;
    }
}

auto BlendOp(DWORD op) noexcept -> GLenum
{
    switch (op)
    {
        case D3DBLENDOP_SUBTRACT:
            return GL_FUNC_SUBTRACT;
        case D3DBLENDOP_REVSUBTRACT:
            return GL_FUNC_REVERSE_SUBTRACT;
        case D3DBLENDOP_MIN:
            return GL_MIN;
        case D3DBLENDOP_MAX:
            return GL_MAX;
        default:
            return GL_FUNC_ADD;
    }
}

auto StencilOp(DWORD op) noexcept -> GLenum
{
    switch (op)
    {
        case D3DSTENCILOP_ZERO:
            return GL_ZERO;
        case D3DSTENCILOP_REPLACE:
            return GL_REPLACE;
        case D3DSTENCILOP_INCRSAT:
            return GL_INCR;
        case D3DSTENCILOP_DECRSAT:
            return GL_DECR;
        case D3DSTENCILOP_INVERT:
            return GL_INVERT;
        case D3DSTENCILOP_INCR:
            return GL_INCR_WRAP;
        case D3DSTENCILOP_DECR:
            return GL_DECR_WRAP;
        default:
            return GL_KEEP;
    }
}

void Enable(GLenum capability, bool on)
{
    if (on)
    {
        glEnable(capability);
    }
    else
    {
        glDisable(capability);
    }
}

void ApplyStates(const DeviceState& state)
{
    if (g_appliedValid && g_applied.renderStates == state.renderStates && std::memcmp(&g_applied.viewport, &state.viewport, sizeof(D3DVIEWPORT8)) == 0)
    {
        return;
    }

    g_applied      = { state.renderStates, state.viewport };
    g_appliedValid = true;

    const auto& viewport = state.viewport;
    glViewport(viewport.X, viewport.Y, viewport.Width, viewport.Height);
    glDepthRangef(viewport.MinZ, viewport.MaxZ);

    const auto& rs = state.renderStates;
    Enable(GL_DEPTH_TEST, rs[D3DRS_ZENABLE] != D3DZB_FALSE);
    glDepthMask(rs[D3DRS_ZWRITEENABLE] != 0);
    glDepthFunc(Compare(rs[D3DRS_ZFUNC]));

    Enable(GL_CULL_FACE, rs[D3DRS_CULLMODE] != D3DCULL_NONE);
    if (rs[D3DRS_CULLMODE] == D3DCULL_CW)
    {
        glCullFace(GL_FRONT);
    }
    else
    {
        glCullFace(GL_BACK);
    }

    Enable(GL_BLEND, rs[D3DRS_ALPHABLENDENABLE] != 0);
    if (rs[D3DRS_SRCBLEND] == D3DBLEND_BOTHSRCALPHA)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else if (rs[D3DRS_SRCBLEND] == D3DBLEND_BOTHINVSRCALPHA)
    {
        glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
    }
    else
    {
        glBlendFunc(Blend(rs[D3DRS_SRCBLEND]), Blend(rs[D3DRS_DESTBLEND]));
    }

    glBlendEquation(BlendOp(rs[D3DRS_BLENDOP]));

    const auto mask = rs[D3DRS_COLORWRITEENABLE];
    glColorMask((mask & D3DCOLORWRITEENABLE_RED) != 0, (mask & D3DCOLORWRITEENABLE_GREEN) != 0, (mask & D3DCOLORWRITEENABLE_BLUE) != 0, (mask & D3DCOLORWRITEENABLE_ALPHA) != 0);

    Enable(GL_STENCIL_TEST, rs[D3DRS_STENCILENABLE] != 0);
    glStencilFunc(Compare(rs[D3DRS_STENCILFUNC]), rs[D3DRS_STENCILREF] & 0xFF, rs[D3DRS_STENCILMASK] & 0xFF);
    glStencilOp(
        StencilOp(rs[D3DRS_STENCILFAIL]), StencilOp(rs[D3DRS_STENCILZFAIL]), StencilOp(rs[D3DRS_STENCILPASS]));
    glStencilMask(rs[D3DRS_STENCILWRITEMASK] & 0xFF);

    // D3D8's ZBIAS 0-16 pulls toward the viewer in steps of the depth buffer's resolution.
    const auto bias = rs[D3DRS_ZBIAS];
    Enable(GL_POLYGON_OFFSET_FILL, bias != 0);
    glPolygonOffset(0.0f, -static_cast<float>(bias));
}

// Every texture first: an upload binds its texture on the active unit, which may be one already set.
void BindTextures(const DeviceState& state)
{
    auto textures = std::array<std::pair<GLenum, GLuint>, nulldx::kTextureStages>{};
    for (DWORD stage = 0; stage < nulldx::kTextureStages; ++stage)
    {
        auto* const texture = state.textures[stage].Get();
        auto& [target, id]  = textures[stage];
        target              = GL_TEXTURE_2D;
        if (texture != nullptr)
        {
            id = TextureFor(texture, target);
        }
        else
        {
            id = 0;
        }
    }

    for (DWORD stage = 0; stage < nulldx::kTextureStages; ++stage)
    {
        const auto [target, id] = textures[stage];
        BindTexture(stage, target, id);
        if (id != 0)
        {
            BindSampler(stage, SamplerFor(state.textureStates[stage]));
        }
        else
        {
            BindSampler(stage, 0);
        }
    }
}

auto IndexCount(D3DPRIMITIVETYPE type, UINT primitives) noexcept -> UINT
{
    switch (type)
    {
        case D3DPT_POINTLIST:
            return primitives;
        case D3DPT_LINELIST:
            return primitives * 2;
        case D3DPT_LINESTRIP:
            return primitives + 1;
        case D3DPT_TRIANGLELIST:
            return primitives * 3;
        default:
            return primitives + 2;
    }
}

auto Mode(D3DPRIMITIVETYPE type) noexcept -> GLenum
{
    switch (type)
    {
        case D3DPT_POINTLIST:
            return GL_POINTS;
        case D3DPT_LINELIST:
            return GL_LINES;
        case D3DPT_LINESTRIP:
            return GL_LINE_STRIP;
        case D3DPT_TRIANGLESTRIP:
            return GL_TRIANGLE_STRIP;
        case D3DPT_TRIANGLEFAN:
            return GL_TRIANGLE_FAN;
        default:
            return GL_TRIANGLES;
    }
}

void Stream(GLuint& buffer, GLenum target, const void* data, size_t size)
{
    if (buffer == 0)
    {
        glGenBuffers(1, &buffer);
    }

    BindBuffer(target, buffer);
    glBufferData(target, size, data, GL_STREAM_DRAW);
}

// WebGL2 has no base vertex, so baseVertexIndex moves each attribute's offset instead.
auto BindStreams(const DeviceState& state, const Layout& layout, const DrawCall& call, UINT indices) -> bool
{
    auto used = std::array<bool, kAttributes>{};
    if (call.vertices != nullptr)
    {
        Stream(g_upVertices, GL_ARRAY_BUFFER, call.vertices, static_cast<size_t>(indices) * call.stride);
    }

    for (const auto& element : layout.elements)
    {
        if (call.vertices != nullptr)
        {
            if (element.stream != 0)
            {
                continue;
            }

            used[element.attribute] = true;
            AttributePointer(element.attribute, g_upVertices, element, call.stride, element.offset);
            continue;
        }

        const auto& stream = state.streams[element.stream];
        if (!stream.buffer)
        {
            return false;
        }

        const auto buffer = BufferFor(static_cast<NullVertexBuffer*>(stream.buffer.Get()));
        const auto stride = static_cast<GLsizei>(stream.stride);
        const auto base   = [&]() -> size_t
        {
            if (call.indexed)
            {
                return static_cast<size_t>(state.baseVertexIndex) * stride;
            }

            return 0;
        }();

        used[element.attribute] = true;
        AttributePointer(element.attribute, buffer, element, stride, base + element.offset);
    }

    for (GLuint location = 0; location < kAttributes; ++location)
    {
        if (!used[location])
        {
            AttributeDefault(location);
        }
    }

    return true;
}

} // namespace

void Start(UINT width, UINT height)
{
    const auto context = kr_gl_context(static_cast<int>(width), static_cast<int>(height));
    if (context == 0 || g_active)
    {
        return;
    }

    g_active         = true;
    g_s3tc           = emscripten_webgl_enable_extension(context, "WEBGL_compressed_texture_s3tc");
    auto vertexArray = GLuint{};
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // With y flipped, a triangle D3D sees clockwise is counter-clockwise in GL's window coordinates.
    glFrontFace(GL_CCW);
}

void Draw(NullDevice& device, const DrawCall& call)
{
    if (!g_active || !g_drawing || call.primitives == 0 || !BindTarget(device))
    {
        return;
    }

    const auto& state  = device.State();
    const auto* layout = UseProgram(device);
    if (layout == nullptr)
    {
        return;
    }

    ApplyStates(state);
    BindTextures(state);

    const auto indices = IndexCount(call.type, call.primitives);
    const auto start   = [&]() -> UINT
    {
        if (call.vertices != nullptr)
        {
            return 0;
        }

        return call.start;
    }();

    if (!BindStreams(state, *layout, call, indices))
    {
        return;
    }

    const auto mode = Mode(call.type);
    if (!call.indexed)
    {
        glDrawArrays(mode, start, indices);
        return;
    }

    auto* const buffer = static_cast<NullIndexBuffer*>(state.indices.Get());
    if (buffer == nullptr)
    {
        return;
    }

    BufferFor(buffer);
    if (buffer->Format() == D3DFMT_INDEX32)
    {
        glDrawElements(mode, indices, GL_UNSIGNED_INT, reinterpret_cast<const void*>(static_cast<size_t>(start) * 4));
    }
    else
    {
        glDrawElements(mode, indices, GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(static_cast<size_t>(start) * 2));
    }
}

// D3D8 clears the viewport, or the parts of the rectangles inside it, whatever the write masks say.
void Clear(NullDevice& device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (!g_active || !g_drawing || !BindTarget(device))
    {
        return;
    }

    g_appliedValid = false;
    auto mask      = GLbitfield{ 0 };
    if ((flags & D3DCLEAR_TARGET) != 0)
    {
        mask |= GL_COLOR_BUFFER_BIT;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const auto rgba = Rgba(color);
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    if (device.DepthStencil() != nullptr && (flags & D3DCLEAR_ZBUFFER) != 0)
    {
        mask |= GL_DEPTH_BUFFER_BIT;
        glDepthMask(GL_TRUE);
        glClearDepthf(z);
    }

    if (device.DepthStencil() != nullptr && (flags & D3DCLEAR_STENCIL) != 0)
    {
        mask |= GL_STENCIL_BUFFER_BIT;
        glStencilMask(0xFF);
        glClearStencil(static_cast<GLint>(stencil));
    }

    if (mask == 0)
    {
        return;
    }

    const auto& viewport = device.State().viewport;
    const auto  left     = static_cast<LONG>(viewport.X);
    const auto  top      = static_cast<LONG>(viewport.Y);
    const auto  right    = static_cast<LONG>(left + viewport.Width);
    const auto  bottom   = static_cast<LONG>(top + viewport.Height);
    glEnable(GL_SCISSOR_TEST);
    if (count == 0 || rects == nullptr)
    {
        glScissor(left, top, right - left, bottom - top);
        glClear(mask);
    }

    for (DWORD i = 0; rects != nullptr && i < count; ++i)
    {
        const auto x1 = std::max(left, rects[i].x1);
        const auto y1 = std::max(top, rects[i].y1);
        const auto x2 = std::min(right, rects[i].x2);
        const auto y2 = std::min(bottom, rects[i].y2);
        if (x2 <= x1 || y2 <= y1)
        {
            continue;
        }

        glScissor(x1, y1, x2 - x1, y2 - y1);
        glClear(mask);
    }

    glDisable(GL_SCISSOR_TEST);
}

// The back buffer is stored top row first, the canvas bottom row first.
void Present(NullDevice& device)
{
    auto* const back = device.BackBuffer();
    if (!g_active || !g_drawing || ReadFramebuffer(back) == 0)
    {
        return;
    }

    const auto width  = static_cast<GLint>(back->Desc().Width);
    const auto height = static_cast<GLint>(back->Desc().Height);
    BindDrawFramebuffer(0);
    glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

namespace
{

// Destinations with a copy on its way back.
std::vector<NullSurface*> g_reading;

void Arrive(NullSurface* to)
{
    auto& back = to->readback;
    if (back.fence == nullptr)
    {
        return;
    }

    auto status = GLint{ GL_UNSIGNALED };
    glGetSynciv(back.fence, GL_SYNC_STATUS, 1, nullptr, &status);
    if (status != GL_SIGNALED)
    {
        return;
    }

    glDeleteSync(back.fence);
    back.fence  = nullptr;
    auto pixels = std::vector<uint32_t>(static_cast<size_t>(back.width) * back.height);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, back.buffer);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, pixels.size() * 4, pixels.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    auto* const bits = to->Bits();
    for (LONG row = 0; row < back.height; ++row)
    {
        auto* const out = reinterpret_cast<uint32_t*>(bits + (back.y + row) * to->Pitch()) + back.x;
        for (LONG column = 0; column < back.width; ++column)
        {
            out[column] = SwapRedBlue(pixels[row * back.width + column]);
        }
    }
}

} // namespace

Readback::~Readback()
{
    g_reading.erase(std::remove_if(g_reading.begin(), g_reading.end(), [this](NullSurface* surface)
                                   {
                                       return &surface->readback == this;
                                   }),
                    g_reading.end());
    if (fence != nullptr)
    {
        glDeleteSync(fence);
    }

    if (buffer != 0)
    {
        glDeleteBuffers(1, &buffer);
    }
}

// A read waits for every queued command, so it runs at the scene's start; fences signal only between host tasks, so a
// copy arrives no later than it would in Copy.
void BeginScene()
{
    if (!g_active)
    {
        return;
    }

    for (auto* const to : g_reading)
    {
        Arrive(to);
    }

    g_reading.erase(std::remove_if(g_reading.begin(), g_reading.end(), [](NullSurface* surface)
                                   {
                                       return surface->readback.fence == nullptr;
                                   }),
                    g_reading.end());
}

// The client reads back 32-bit targets only, for occlusion queries. Waiting on the GPU costs milliseconds a query, so a
// destination gets its last copy whose pixels have arrived, a frame or so late, and reads as all visible until then.
auto Copy(NullSurface* from, const RECT* rects, UINT count, NullSurface* to, const POINT* points) -> bool
{
    if (!g_active || ReadFramebuffer(from) == 0)
    {
        return false;
    }

    if (nulldx::LayoutOf(to->Desc().Format).blockBytes != 4)
    {
        return true;
    }

    auto& back = to->readback;
    if (back.buffer == 0)
    {
        glGenBuffers(1, &back.buffer);
        std::memset(to->Bits(), 0xFF, to->Desc().Size);
    }

    Arrive(to);
    // One copy in flight per destination; the client copies one rectangle.
    const auto rect = [&]() -> RECT
    {
        if (rects != nullptr)
        {
            return rects[0];
        }

        return RECT{ 0, 0, static_cast<LONG>(from->Desc().Width), static_cast<LONG>(from->Desc().Height) };
    }();

    const auto at = [&]() -> POINT
    {
        if (points != nullptr)
        {
            return points[0];
        }

        return POINT{ rect.left, rect.top };
    }();

    const auto width  = std::min<LONG>(rect.right - rect.left, to->Desc().Width - at.x);
    const auto height = std::min<LONG>(rect.bottom - rect.top, to->Desc().Height - at.y);
    if (back.fence == nullptr && (rects == nullptr || count > 0) && width > 0 && height > 0 && at.x >= 0 && at.y >= 0)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, back.buffer);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(width) * height * 4, nullptr, GL_STREAM_READ);
        glReadPixels(rect.left, rect.top, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        back.fence  = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        back.x      = at.x;
        back.y      = at.y;
        back.width  = width;
        back.height = height;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (std::find(g_reading.begin(), g_reading.end(), to) == g_reading.end())
        {
            g_reading.push_back(to);
        }
    }

    return true;
}

} // namespace gl

// Textures and buffers upload when a draw first needs them, so the draws skipped meanwhile leave nothing stale.
extern "C" void kr_gl_draw(bool on)
{
    gl::g_drawing = on;
}
