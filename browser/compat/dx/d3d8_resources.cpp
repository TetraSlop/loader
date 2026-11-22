#include "d3d8_null.h"

#include "../gl/gl.h"

#include <algorithm>
#include <cstring>

namespace nulldx
{

namespace
{

// Byte offset of a rectangle's top left corner; DXT rectangles start on a block.
auto Offset(D3DFORMAT format, UINT pitch, LONG left, LONG top) -> size_t
{
    const auto layout = LayoutOf(format);
    return static_cast<size_t>(top / layout.blockEdge) * pitch + static_cast<size_t>(left / layout.blockEdge) * layout.blockBytes;
}

constexpr auto Shrink(UINT size, UINT level) -> UINT
{
    return std::max(1u, size >> level);
}

// Copies whole blocks, so DXT rectangles round out to their 4x4 blocks.
auto CopyRegion(NullSurface* source, const RECT& rect, NullSurface* destination, POINT at) -> void
{
    const auto format = source->Desc().Format;
    const auto edge   = LayoutOf(format).blockEdge;
    const auto width  = std::min<LONG>(std::min<LONG>(rect.right, source->Desc().Width) - rect.left, destination->Desc().Width - at.x);
    const auto height = std::min<LONG>(std::min<LONG>(rect.bottom, source->Desc().Height) - rect.top, destination->Desc().Height - at.y);
    if (rect.left < 0 || rect.top < 0 || at.x < 0 || at.y < 0 || width <= 0 || height <= 0)
    {
        return;
    }

    const auto  rowBytes   = RowPitch(format, width);
    const auto  rows       = BlockRows(format, height);
    const auto  blockBytes = LayoutOf(format).blockBytes;
    const auto* from       = source->Bits() + rect.top / edge * source->Pitch() + rect.left / edge * blockBytes;
    auto* const to         = destination->Bits() + at.y / edge * destination->Pitch() + at.x / edge * blockBytes;
    for (UINT row = 0; row < rows; ++row)
    {
        std::memcpy(to + row * destination->Pitch(), from + row * source->Pitch(), rowBytes);
    }
}

} // namespace

NullSurface::NullSurface(NullDevice* device, IUnknown* container, const D3DSURFACE_DESC& desc)
: Child(device)
, container_(container)
, desc_(desc)
{
}

ULONG NullSurface::AddRef()
{
    if (container_ != nullptr)
    {
        return container_->AddRef();
    }

    return Child::AddRef();
}

ULONG NullSurface::Release()
{
    if (container_ != nullptr)
    {
        return container_->Release();
    }

    return Child::Release();
}

HRESULT NullSurface::GetContainer(REFIID riid, void** container)
{
    if (container_ != nullptr)
    {
        return container_->QueryInterface(riid, container);
    }

    return device()->QueryInterface(riid, container);
}

HRESULT NullSurface::GetDesc(D3DSURFACE_DESC* desc)
{
    return Store(desc, desc_, D3DERR_INVALIDCALL);
}

auto NullSurface::Bits() -> BYTE*
{
    if (bits_.empty())
    {
        bits_.resize(desc_.Size);
    }

    glDirty = true;
    return bits_.data();
}

HRESULT NullSurface::LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    if (locked == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    const auto offset = [&]() -> size_t
    {
        if (rect == nullptr)
        {
            return 0;
        }

        return Offset(desc_.Format, Pitch(), rect->left, rect->top);
    }();

    locked->Pitch = static_cast<INT>(Pitch());
    locked->pBits = Bits() + offset;
    return D3D_OK;
}

HRESULT NullSurface::UnlockRect()
{
    return D3D_OK;
}

NullTexture::NullTexture(
    NullDevice* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
: BaseTexture(device, D3DRTYPE_TEXTURE, levels)
{
    for (UINT level = 0; level < levels; ++level)
    {
        const auto desc = SurfaceDesc(format, usage, pool, Shrink(width, level), Shrink(height, level));
        levels_.push_back(std::make_unique<NullSurface>(device, this, desc));
    }
}

HRESULT NullTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc)
{
    if (Level(level) == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    return Level(level)->GetDesc(desc);
}

HRESULT NullTexture::GetSurfaceLevel(UINT level, IDirect3DSurface8** surface)
{
    if (surface == nullptr || Level(level) == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *surface = Level(level);
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT NullTexture::LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    if (Level(level) == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    return Level(level)->LockRect(locked, rect, flags);
}

HRESULT NullTexture::UnlockRect(UINT level)
{
    if (Level(level) == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    return D3D_OK;
}

NullCubeTexture::NullCubeTexture(
    NullDevice* device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
: BaseTexture(device, D3DRTYPE_CUBETEXTURE, levels)
, levelCount_(levels)
{
    for (UINT face = 0; face < 6; ++face)
    {
        for (UINT level = 0; level < levels; ++level)
        {
            const auto desc = SurfaceDesc(format, usage, pool, Shrink(edge, level), Shrink(edge, level));
            faces_.push_back(std::make_unique<NullSurface>(device, this, desc));
        }
    }
}

auto NullCubeTexture::Face(D3DCUBEMAP_FACES face, UINT level) const -> NullSurface*
{
    if (static_cast<UINT>(face) >= 6 || level >= levelCount_)
    {
        return nullptr;
    }

    return faces_[face * levelCount_ + level].get();
}

HRESULT NullCubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc)
{
    auto* const surface = Face(D3DCUBEMAP_FACE_POSITIVE_X, level);
    if (surface == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    return surface->GetDesc(desc);
}

HRESULT NullCubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface)
{
    if (surface == nullptr || Face(face, level) == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *surface = Face(face, level);
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT NullCubeTexture::LockRect(
    D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    auto* const surface = Face(face, level);
    if (surface == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    return surface->LockRect(locked, rect, flags);
}

HRESULT NullCubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level)
{
    if (Face(face, level) == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    return D3D_OK;
}

namespace
{

auto LockRange(std::vector<BYTE>& data, gl::Dirty& dirty, UINT offset, UINT size, BYTE** out) -> HRESULT
{
    if (out == nullptr || offset > data.size() || (size != 0 && size > data.size() - offset))
    {
        return D3DERR_INVALIDCALL;
    }

    const auto end = [&]() -> size_t
    {
        if (size == 0)
        {
            return data.size();
        }

        return offset + size;
    }();

    *out = data.data() + offset;
    dirty.Mark(offset, end);
    return D3D_OK;
}

} // namespace

NullVertexBuffer::NullVertexBuffer(NullDevice* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
: Resource(device, D3DRTYPE_VERTEXBUFFER)
, desc_{ D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, usage, pool, length, fvf }
, data_(length)
{
}

HRESULT NullVertexBuffer::Lock(UINT offset, UINT size, BYTE** data, DWORD flags)
{
    return LockRange(data_, glDirty, offset, size, data);
}

HRESULT NullVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC* desc)
{
    return Store(desc, desc_, D3DERR_INVALIDCALL);
}

NullIndexBuffer::NullIndexBuffer(NullDevice* device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
: Resource(device, D3DRTYPE_INDEXBUFFER)
, desc_{ format, D3DRTYPE_INDEXBUFFER, usage, pool, length }
, data_(length)
{
}

HRESULT NullIndexBuffer::Lock(UINT offset, UINT size, BYTE** data, DWORD flags)
{
    return LockRange(data_, glDirty, offset, size, data);
}

HRESULT NullIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC* desc)
{
    return Store(desc, desc_, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::CreateTexture(
    UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture)
{
    if (texture == nullptr || width == 0 || height == 0 || LayoutOf(format).blockBytes == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    *texture = new NullTexture(this, width, height, LevelCount(width, height, 1, levels), usage, format, pool);
    return D3D_OK;
}

HRESULT NullDevice::CreateCubeTexture(
    UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8** texture)
{
    if (texture == nullptr || edge == 0 || LayoutOf(format).blockBytes == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    *texture = new NullCubeTexture(this, edge, LevelCount(edge, edge, 1, levels), usage, format, pool);
    return D3D_OK;
}

HRESULT NullDevice::CreateVertexBuffer(
    UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** buffer)
{
    if (buffer == nullptr || length == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    *buffer = new NullVertexBuffer(this, length, usage, fvf, pool);
    return D3D_OK;
}

HRESULT NullDevice::CreateIndexBuffer(
    UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** buffer)
{
    if (buffer == nullptr || length == 0 || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32))
    {
        return D3DERR_INVALIDCALL;
    }

    *buffer = new NullIndexBuffer(this, length, usage, format, pool);
    return D3D_OK;
}

HRESULT NullDevice::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, BOOL lockable, IDirect3DSurface8** surface)
{
    if (surface == nullptr || width == 0 || height == 0 || LayoutOf(format).blockBytes == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    const auto desc = SurfaceDesc(format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, width, height, multiSample);
    *surface        = new NullSurface(this, nullptr, desc);
    return D3D_OK;
}

HRESULT NullDevice::CreateDepthStencilSurface(
    UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, IDirect3DSurface8** surface)
{
    if (surface == nullptr || width == 0 || height == 0 || !IsDepthFormat(format))
    {
        return D3DERR_INVALIDCALL;
    }

    const auto desc = SurfaceDesc(format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, width, height, multiSample);
    *surface        = new NullSurface(this, nullptr, desc);
    return D3D_OK;
}

HRESULT NullDevice::CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface)
{
    if (surface == nullptr || width == 0 || height == 0 || LayoutOf(format).blockBytes == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    *surface = new NullSurface(this, nullptr, SurfaceDesc(format, 0, D3DPOOL_SYSTEMMEM, width, height));
    return D3D_OK;
}

// tmweb: D3DX 8 loads every texture into a system-memory copy and updates the real one from it. Kestrel never
// called this, so it was a stub; TM.dll's statically linked D3DX does for each texture it creates. Plain textures only.
HRESULT NullDevice::UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination)
{
    if (source == nullptr || destination == nullptr || source->GetType() != D3DRTYPE_TEXTURE || destination->GetType() != D3DRTYPE_TEXTURE)
    {
        return D3DERR_INVALIDCALL;
    }

    auto* const from   = static_cast<IDirect3DTexture8*>(source);
    auto* const to     = static_cast<IDirect3DTexture8*>(destination);
    const auto  levels = std::min(from->GetLevelCount(), to->GetLevelCount());
    for (UINT level = 0; level < levels; ++level)
    {
        IDirect3DSurface8* a = nullptr;
        IDirect3DSurface8* b = nullptr;
        from->GetSurfaceLevel(level, &a);
        to->GetSurfaceLevel(level, &b);
        auto* const fromLevel = static_cast<NullSurface*>(a);
        auto* const toLevel   = static_cast<NullSurface*>(b);
        const auto  same      = fromLevel->Desc().Format == toLevel->Desc().Format && fromLevel->Desc().Width == toLevel->Desc().Width && fromLevel->Desc().Height == toLevel->Desc().Height;
        if (same)
        {
            const auto whole = RECT{ 0, 0, static_cast<LONG>(fromLevel->Desc().Width), static_cast<LONG>(fromLevel->Desc().Height) };
            CopyRegion(fromLevel, whole, toLevel, POINT{ 0, 0 });
        }
        a->Release();
        b->Release();
        if (!same)
        {
            return D3DERR_INVALIDCALL;
        }
    }

    return D3D_OK;
}

HRESULT NullDevice::CopyRects(
    IDirect3DSurface8* source, const RECT* rects, UINT count, IDirect3DSurface8* destination, const POINT* points)
{
    if (source == nullptr || destination == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    auto* const from = static_cast<NullSurface*>(source);
    auto* const to   = static_cast<NullSurface*>(destination);
    if (from->Desc().Format != to->Desc().Format)
    {
        return D3DERR_INVALIDCALL;
    }

    if (gl::Copy(from, rects, count, to, points))
    {
        return D3D_OK;
    }

    // Without GL the client copies a render target only to read occlusion: all visible, so actors still draw.
    if ((from->Desc().Usage & D3DUSAGE_RENDERTARGET) != 0)
    {
        std::memset(to->Bits(), 0xFF, to->Desc().Size);
        return D3D_OK;
    }

    if (rects == nullptr)
    {
        const auto whole = RECT{ 0, 0, static_cast<LONG>(from->Desc().Width), static_cast<LONG>(from->Desc().Height) };
        const auto at    = [&]() -> POINT
        {
            if (points == nullptr)
            {
                return POINT{ 0, 0 };
            }

            return points[0];
        }();

        CopyRegion(from, whole, to, at);
        return D3D_OK;
    }

    for (UINT i = 0; i < count; ++i)
    {
        const auto at = [&]() -> POINT
        {
            if (points == nullptr)
            {
                return POINT{ rects[i].left, rects[i].top };
            }

            return points[i];
        }();

        CopyRegion(from, rects[i], to, at);
    }

    return D3D_OK;
}

} // namespace nulldx
