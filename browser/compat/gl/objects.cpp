// GL objects for the device's resources. The null device's CPU copy stays the source of truth for what the client fills,
// uploaded when a draw finds it changed; render targets and depth buffers live on the GPU only. Every target is stored
// top row first, as D3D8 lays out texels, so a scene drawn into a texture samples the right way up; programs.cpp flips y.
#include "internal.h"

#include "../dx/d3dx8_pixels.h"

#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>

namespace gl
{

using nulldx::NullCubeTexture;
using nulldx::NullIndexBuffer;
using nulldx::NullTexture;
using nulldx::NullVertexBuffer;

namespace
{

struct Attachment
{
    GLuint id     = 0;
    GLenum target = 0; // a texture or cube face target, or GL_RENDERBUFFER
    GLint  level  = 0;

    auto operator==(const Attachment& other) const noexcept -> bool
    {
        return id == other.id && target == other.target && level == other.level;
    }
};

GLuint     g_drawFramebuffer;
GLuint     g_readFramebuffer;
Attachment g_color;
Attachment g_depth;
bool       g_complete;

auto OnGpu(const D3DSURFACE_DESC& desc) noexcept -> bool
{
    return (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
}

// GL has no BGRA, and X8 formats keep alpha at 1 as D3D reads it.
void Allocate(GLenum target, GLint level, const D3DSURFACE_DESC& desc)
{
    if (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_R5G6B5 || desc.Format == D3DFMT_X1R5G5B5)
    {
        glTexImage2D(target, level, GL_RGB8, desc.Width, desc.Height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    }
    else
    {
        glTexImage2D(target, level, GL_RGBA8, desc.Width, desc.Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
}

auto Compressed(D3DFORMAT format) noexcept -> GLenum
{
    if (!g_s3tc)
    {
        return 0;
    }

    switch (format)
    {
        case D3DFMT_DXT1:
            return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
            return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
            return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        default:
            return 0;
    }
}

// WEBGL_compressed_texture_s3tc takes whole blocks, and below level 0 the 1 and 2 texel sides as well. Levels of mixed
// formats leave a texture incomplete, so one level that cannot go compressed sends them all decoded.
auto UploadsCompressed(const D3DSURFACE_DESC& top, DWORD levels) noexcept -> bool
{
    if (Compressed(top.Format) == 0)
    {
        return false;
    }

    for (DWORD level = 0; level < levels; ++level)
    {
        const auto blocks = [&](UINT size)
        {
            const auto side = std::max(1u, size >> level);
            return side % 4 == 0 || (level > 0 && side <= 2);
        };

        if (!blocks(top.Width) || !blocks(top.Height))
        {
            return false;
        }
    }

    return true;
}

// Uncompressed formats go through D3DX's decoder to ARGB, then to the RGBA bytes GL takes.
void Upload(GLenum target, GLint level, NullSurface* surface, bool compressed)
{
    const auto& desc = surface->Desc();
    const auto* bits = surface->Bits();
    surface->glDirty = false;
    if (compressed)
    {
        glCompressedTexImage2D(target, level, Compressed(desc.Format), desc.Width, desc.Height, 0, desc.Size, bits);
        return;
    }

    auto       image = nulldx::Image{};
    const auto whole = RECT{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
    auto       rgba  = std::vector<uint32_t>(desc.Width * desc.Height, 0xFFFFFFFF);
    if (nulldx::Decode(desc.Format, bits, surface->Pitch(), nullptr, whole, image))
    {
        for (size_t i = 0; i < rgba.size(); ++i)
        {
            rgba[i] = SwapRedBlue(image.pixels[i]);
        }
    }
    else
    {
        static auto told = false;
        if (!told)
        {
            std::fprintf(stderr, "gl: no decoder for texture format %u\n", static_cast<unsigned>(desc.Format));
        }

        told = true;
    }

    glTexImage2D(target, level, GL_RGBA8, desc.Width, desc.Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

// Makes the texture, bound; returns whether it was new.
auto Create(gl::Name& name, GLenum target, DWORD levels) -> bool
{
    if (name.id != 0)
    {
        return false;
    }

    glGenTextures(1, &name.id);
    name.target = target;
    BindTexture(target, name.id);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels) - 1);
    return true;
}

// Gives a GPU-only level its storage once, and uploads a level the client changed.
void Fill(GLenum bind, GLuint id, GLenum target, GLint level, NullSurface* surface, bool created, bool compressed)
{
    if (OnGpu(surface->Desc()))
    {
        if (created)
        {
            Allocate(target, level, surface->Desc());
        }
    }
    else if (surface->glDirty)
    {
        BindTexture(bind, id);
        Upload(target, level, surface, compressed);
    }
}

auto Texture2D(NullTexture* texture) -> GLuint
{
    const auto created    = Create(texture->gl, GL_TEXTURE_2D, texture->GetLevelCount());
    const auto compressed = UploadsCompressed(texture->Level(0)->Desc(), texture->GetLevelCount());
    for (DWORD level = 0; level < texture->GetLevelCount(); ++level)
    {
        Fill(GL_TEXTURE_2D, texture->gl.id, GL_TEXTURE_2D, level, texture->Level(level), created, compressed);
    }

    return texture->gl.id;
}

auto TextureCube(NullCubeTexture* texture) -> GLuint
{
    const auto created    = Create(texture->gl, GL_TEXTURE_CUBE_MAP, texture->GetLevelCount());
    const auto compressed = UploadsCompressed(texture->Face(D3DCUBEMAP_FACE_POSITIVE_X, 0)->Desc(), texture->GetLevelCount());
    for (UINT face = 0; face < 6; ++face)
    {
        for (DWORD level = 0; level < texture->GetLevelCount(); ++level)
        {
            Fill(GL_TEXTURE_CUBE_MAP, texture->gl.id, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, texture->Face(static_cast<D3DCUBEMAP_FACES>(face), level), created, compressed);
        }
    }

    return texture->gl.id;
}

// A level of its texture, or its own texture or renderbuffer.
auto AttachmentFor(NullSurface* surface) -> Attachment
{
    if (surface == nullptr)
    {
        return {};
    }

    if (auto* const container = surface->Container())
    {
        auto* const base   = static_cast<IDirect3DBaseTexture8*>(container);
        auto        target = GLenum{};
        const auto  id     = TextureFor(base, target);
        for (DWORD level = 0; level < base->GetLevelCount(); ++level)
        {
            if (target == GL_TEXTURE_2D && static_cast<NullTexture*>(base)->Level(level) == surface)
            {
                return { id, target, static_cast<GLint>(level) };
            }

            for (UINT face = 0; target == GL_TEXTURE_CUBE_MAP && face < 6; ++face)
            {
                if (static_cast<NullCubeTexture*>(base)->Face(static_cast<D3DCUBEMAP_FACES>(face), level) == surface)
                {
                    return { id, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, static_cast<GLint>(level) };
                }
            }
        }

        return {};
    }

    const auto& desc = surface->Desc();
    if (!OnGpu(desc))
    {
        return {};
    }

    if (surface->gl.id == 0)
    {
        if (nulldx::IsDepthFormat(desc.Format))
        {
            glGenRenderbuffers(1, &surface->gl.id);
            surface->gl.target = GL_RENDERBUFFER;
            glBindRenderbuffer(GL_RENDERBUFFER, surface->gl.id);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, desc.Width, desc.Height);
        }
        else
        {
            Create(surface->gl, GL_TEXTURE_2D, 1);
            Allocate(GL_TEXTURE_2D, 0, desc);
        }
    }

    return { surface->gl.id, surface->gl.target, 0 };
}

void Attach(GLenum framebuffer, GLenum point, const Attachment& attachment)
{
    if (attachment.target == GL_RENDERBUFFER)
    {
        glFramebufferRenderbuffer(framebuffer, point, GL_RENDERBUFFER, attachment.id);
    }
    else if (attachment.target != 0)
    {
        glFramebufferTexture2D(framebuffer, point, attachment.target, attachment.id, attachment.level);
    }
    else
    {
        glFramebufferTexture2D(framebuffer, point, GL_TEXTURE_2D, attachment.id, attachment.level);
    }
}

auto UploadBuffer(gl::Name& name, gl::Dirty& dirty, const std::vector<BYTE>& data, GLenum target) -> GLuint
{
    if (name.id == 0)
    {
        glGenBuffers(1, &name.id);
        name.target = target;
        BindBuffer(target, name.id);
        glBufferData(target, data.size(), nullptr, GL_DYNAMIC_DRAW);
    }
    else
    {
        BindBuffer(target, name.id);
    }

    const auto end = std::min(dirty.end, data.size());
    if (dirty.begin < end)
    {
        glBufferSubData(target, dirty.begin, end - dirty.begin, data.data() + dirty.begin);
    }

    dirty.Clear();
    return name.id;
}

auto Wrap(DWORD address) noexcept -> GLint
{
    switch (address)
    {
        case D3DTADDRESS_MIRROR:
        case D3DTADDRESS_MIRRORONCE:
            return GL_MIRRORED_REPEAT;
        case D3DTADDRESS_CLAMP:
        case D3DTADDRESS_BORDER: // WebGL has no border colour
            return GL_CLAMP_TO_EDGE;
        default:
            return GL_REPEAT;
    }
}

} // namespace

Name::~Name()
{
    if (id == 0)
    {
        return;
    }

    if (target == GL_RENDERBUFFER)
    {
        glDeleteRenderbuffers(1, &id);
    }
    else if (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)
    {
        glDeleteBuffers(1, &id);
    }
    else
    {
        glDeleteTextures(1, &id);
    }

    // GL reuses names, so a new object could pass for an attachment or binding this one held.
    g_color = {};
    g_depth = {};
    ForgetBindings();
}

auto TextureFor(IDirect3DBaseTexture8* texture, GLenum& target) -> GLuint
{
    switch (texture->GetType())
    {
        case D3DRTYPE_TEXTURE:
            target = GL_TEXTURE_2D;
            return Texture2D(static_cast<NullTexture*>(texture));
        case D3DRTYPE_CUBETEXTURE:
            target = GL_TEXTURE_CUBE_MAP;
            return TextureCube(static_cast<NullCubeTexture*>(texture));
        default:
            target = GL_TEXTURE_2D;
            return 0;
    }
}

auto SamplerFor(const std::array<DWORD, nulldx::kTextureStageStates>& stage) -> GLuint
{
    const auto  mag      = stage[D3DTSS_MAGFILTER];
    const auto  min      = stage[D3DTSS_MINFILTER];
    const auto  mip      = stage[D3DTSS_MIPFILTER];
    const auto  key      = static_cast<uint32_t>((std::min<DWORD>(mag, 3) << 0) | (std::min<DWORD>(min, 3) << 2) | (std::min<DWORD>(mip, 3) << 4) | (std::min<DWORD>(stage[D3DTSS_ADDRESSU], 7) << 6) | (std::min<DWORD>(stage[D3DTSS_ADDRESSV], 7) << 9) | (std::min<DWORD>(stage[D3DTSS_ADDRESSW], 7) << 12));
    static auto samplers = std::map<uint32_t, GLuint>{};
    auto&       sampler  = samplers[key];
    if (sampler != 0)
    {
        return sampler;
    }

    glGenSamplers(1, &sampler);
    const auto linear    = min >= D3DTEXF_LINEAR;
    const auto minFilter = [&]() -> GLint
    {
        if (mip == D3DTEXF_POINT)
        {
            if (linear)
            {
                return GL_LINEAR_MIPMAP_NEAREST;
            }

            return GL_NEAREST_MIPMAP_NEAREST;
        }

        if (mip >= D3DTEXF_LINEAR)
        {
            if (linear)
            {
                return GL_LINEAR_MIPMAP_LINEAR;
            }

            return GL_NEAREST_MIPMAP_LINEAR;
        }

        if (linear)
        {
            return GL_LINEAR;
        }

        return GL_NEAREST;
    }();

    const auto magFilter = [&]() -> GLint
    {
        if (mag >= D3DTEXF_LINEAR)
        {
            return GL_LINEAR;
        }

        return GL_NEAREST;
    }();

    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, magFilter);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, Wrap(stage[D3DTSS_ADDRESSU]));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, Wrap(stage[D3DTSS_ADDRESSV]));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, Wrap(stage[D3DTSS_ADDRESSW]));
    return sampler;
}

auto BufferFor(NullVertexBuffer* buffer) -> GLuint
{
    return UploadBuffer(buffer->gl, buffer->glDirty, buffer->Data(), GL_ARRAY_BUFFER);
}

auto BufferFor(NullIndexBuffer* buffer) -> GLuint
{
    return UploadBuffer(buffer->gl, buffer->glDirty, buffer->Data(), GL_ELEMENT_ARRAY_BUFFER);
}

auto BindTarget(const NullDevice& device) -> bool
{
    if (g_drawFramebuffer == 0)
    {
        glGenFramebuffers(1, &g_drawFramebuffer);
    }

    BindDrawFramebuffer(g_drawFramebuffer);
    const auto color = AttachmentFor(device.RenderTarget());
    const auto depth = AttachmentFor(device.DepthStencil());
    if (color == g_color && depth == g_depth)
    {
        return g_complete;
    }

    if (!(color == g_color))
    {
        Attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color);
    }

    if (!(depth == g_depth))
    {
        Attach(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth);
    }

    g_color = color;
    g_depth = depth;
    // glCheckFramebufferStatus waits for the GPU and the client switches targets several times a frame, so each pair is
    // asked once; the client renders only to formats GL takes, so a reused id never needs asking again.
    static auto complete      = std::map<std::tuple<GLuint, GLenum, GLint, GLuint, GLenum, GLint>, bool>{};
    const auto  key           = std::make_tuple(color.id, color.target, color.level, depth.id, depth.target, depth.level);
    const auto [known, added] = complete.try_emplace(key);
    if (added)
    {
        known->second = color.id != 0 && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    g_complete = known->second;
    return g_complete;
}

auto ReadFramebuffer(NullSurface* surface) -> GLuint
{
    const auto color = AttachmentFor(surface);
    if (color.id == 0 || color.target == GL_RENDERBUFFER)
    {
        return 0;
    }

    if (g_readFramebuffer == 0)
    {
        glGenFramebuffers(1, &g_readFramebuffer);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_readFramebuffer);
    Attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color);
    return g_readFramebuffer;
}

} // namespace gl
