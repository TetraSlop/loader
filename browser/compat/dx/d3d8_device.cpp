#include "d3d8_null.h"

#include "../gl/gl.h"

#include <algorithm>

namespace nulldx
{

NullDevice::NullDevice(
    NullDirect3D* direct3D, const D3DDEVICE_CREATION_PARAMETERS& creation, const D3DPRESENT_PARAMETERS& params)
: direct3D_(direct3D)
, creation_(creation)
, params_(params)
{
    direct3D_->AddRef();
    gl::Start(params_.BackBufferWidth, params_.BackBufferHeight);
    CreateImplicitSurfaces();
    state_ = DefaultState(params_);
}

NullDevice::~NullDevice()
{
    direct3D_->Release();
}

auto NullDevice::CreateImplicitSurfaces() -> void
{
    const auto color = SurfaceDesc(params_.BackBufferFormat, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, params_.BackBufferWidth, params_.BackBufferHeight, params_.MultiSampleType);
    backBuffer_      = Ref<NullSurface>::Adopt(new NullSurface(this, nullptr, color));
    autoDepthStencil_.Reset();
    if (params_.EnableAutoDepthStencil)
    {
        const auto depth  = SurfaceDesc(params_.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, params_.BackBufferWidth, params_.BackBufferHeight, params_.MultiSampleType);
        autoDepthStencil_ = Ref<NullSurface>::Adopt(new NullSurface(this, nullptr, depth));
    }

    renderTarget_.Reset(backBuffer_.Get());
    depthStencil_.Reset(autoDepthStencil_.Get());
}

HRESULT NullDevice::GetDirect3D(IDirect3D8** direct3D)
{
    if (direct3D == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    direct3D_->AddRef();
    *direct3D = direct3D_;
    return D3D_OK;
}

HRESULT NullDevice::GetDeviceCaps(D3DCAPS8* caps)
{
    return direct3D_->GetDeviceCaps(creation_.AdapterOrdinal, creation_.DeviceType, caps);
}

HRESULT NullDevice::GetDisplayMode(D3DDISPLAYMODE* mode)
{
    if (params_.Windowed)
    {
        return Store(mode, NullDirect3D::DesktopMode(), D3DERR_INVALIDCALL);
    }

    const auto fullScreen = D3DDISPLAYMODE{ params_.BackBufferWidth, params_.BackBufferHeight, params_.FullScreen_RefreshRateInHz, params_.BackBufferFormat };
    return Store(mode, fullScreen, D3DERR_INVALIDCALL);
}

HRESULT
NullDevice::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params)
{
    return Store(params, creation_, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::Reset(D3DPRESENT_PARAMETERS* params)
{
    if (params == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    CompletePresentParameters(*params);
    params_ = *params;
    gl::Start(params_.BackBufferWidth, params_.BackBufferHeight);
    recording_.reset();
    CreateImplicitSurfaces();
    state_ = DefaultState(params_);
    return D3D_OK;
}

HRESULT NullDevice::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** surface)
{
    if (surface == nullptr || index >= params_.BackBufferCount)
    {
        return D3DERR_INVALIDCALL;
    }

    *surface = backBuffer_.Share();
    return D3D_OK;
}

HRESULT NullDevice::GetRasterStatus(D3DRASTER_STATUS* status)
{
    return Store(status, { FALSE, 0 }, D3DERR_INVALIDCALL);
}

// D3D8 resets the viewport to the new target.
HRESULT NullDevice::SetRenderTarget(IDirect3DSurface8* renderTarget, IDirect3DSurface8* depthStencil)
{
    if (renderTarget != nullptr)
    {
        renderTarget_.Reset(renderTarget);
    }

    depthStencil_.Reset(depthStencil);
    D3DSURFACE_DESC desc;
    renderTarget_->GetDesc(&desc);
    state_.viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    return D3D_OK;
}

HRESULT NullDevice::GetRenderTarget(IDirect3DSurface8** renderTarget)
{
    if (renderTarget == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *renderTarget = renderTarget_.Share();
    return D3D_OK;
}

HRESULT NullDevice::GetDepthStencilSurface(IDirect3DSurface8** depthStencil)
{
    if (depthStencil == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *depthStencil = depthStencil_.Share();
    if (*depthStencil == nullptr)
    {
        return D3DERR_NOTFOUND;
    }

    return D3D_OK;
}

HRESULT NullDevice::SetTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix)
{
    if (matrix == nullptr || static_cast<DWORD>(type) >= kTransforms)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().transforms[type] = *matrix;
    if (auto* const mask = Recorded())
    {
        mask->transforms.set(type);
    }

    return D3D_OK;
}

HRESULT NullDevice::GetTransform(D3DTRANSFORMSTATETYPE type, D3DMATRIX* matrix)
{
    if (matrix == nullptr || static_cast<DWORD>(type) >= kTransforms)
    {
        return D3DERR_INVALIDCALL;
    }

    *matrix = state_.transforms[type];
    return D3D_OK;
}

HRESULT NullDevice::SetViewport(const D3DVIEWPORT8* viewport)
{
    if (viewport == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().viewport = *viewport;
    if (auto* const mask = Recorded())
    {
        mask->viewport = true;
    }

    return D3D_OK;
}

HRESULT NullDevice::GetViewport(D3DVIEWPORT8* viewport)
{
    return Store(viewport, state_.viewport, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::SetMaterial(const D3DMATERIAL8* material)
{
    if (material == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().material = *material;
    if (auto* const mask = Recorded())
    {
        mask->material = true;
    }

    return D3D_OK;
}

HRESULT NullDevice::GetMaterial(D3DMATERIAL8* material)
{
    return Store(material, state_.material, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::SetLight(DWORD index, const D3DLIGHT8* light)
{
    if (light == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    const auto inserted          = Target().lights.try_emplace(index, Light{ *light, FALSE });
    inserted.first->second.light = *light;
    if (auto* const mask = Recorded())
    {
        mask->lights.insert(index);
    }

    return D3D_OK;
}

HRESULT NullDevice::GetLight(DWORD index, D3DLIGHT8* light)
{
    const auto found = state_.lights.find(index);
    if (light == nullptr || found == state_.lights.end())
    {
        return D3DERR_INVALIDCALL;
    }

    *light = found->second.light;
    return D3D_OK;
}

// Enabling a light that was never set gives it D3D8's default: a white directional light down +Z.
HRESULT NullDevice::LightEnable(DWORD index, BOOL enable)
{
    auto light                     = D3DLIGHT8{};
    light.Type                     = D3DLIGHT_DIRECTIONAL;
    light.Diffuse                  = { 1.0f, 1.0f, 1.0f, 0.0f };
    light.Direction                = { 0.0f, 0.0f, 1.0f };
    const auto inserted            = Target().lights.try_emplace(index, Light{ light, FALSE });
    inserted.first->second.enabled = static_cast<BOOL>(enable != FALSE);
    if (auto* const mask = Recorded())
    {
        mask->lights.insert(index);
    }

    return D3D_OK;
}

HRESULT NullDevice::SetRenderState(D3DRENDERSTATETYPE type, DWORD value)
{
    if (static_cast<DWORD>(type) >= kRenderStates)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().renderStates[type] = value;
    if (auto* const mask = Recorded())
    {
        mask->renderStates.set(type);
    }

    return D3D_OK;
}

HRESULT NullDevice::GetRenderState(D3DRENDERSTATETYPE type, DWORD* value)
{
    if (value == nullptr || static_cast<DWORD>(type) >= kRenderStates)
    {
        return D3DERR_INVALIDCALL;
    }

    *value = state_.renderStates[type];
    return D3D_OK;
}

HRESULT NullDevice::BeginStateBlock()
{
    if (recording_)
    {
        return D3DERR_INVALIDCALL;
    }

    recording_        = std::make_unique<StateBlock>();
    recording_->state = state_;
    return D3D_OK;
}

HRESULT NullDevice::EndStateBlock(DWORD* token)
{
    if (token == nullptr || !recording_)
    {
        return D3DERR_INVALIDCALL;
    }

    *token               = nextStateBlock_++;
    stateBlocks_[*token] = std::move(*recording_);
    recording_.reset();
    return D3D_OK;
}

HRESULT NullDevice::ApplyStateBlock(DWORD token)
{
    const auto block = stateBlocks_.find(token);
    if (block == stateBlocks_.end())
    {
        return D3DERR_INVALIDCALL;
    }

    CopyMasked(block->second.state, state_, block->second.mask);
    return D3D_OK;
}

HRESULT NullDevice::CaptureStateBlock(DWORD token)
{
    const auto block = stateBlocks_.find(token);
    if (block == stateBlocks_.end())
    {
        return D3DERR_INVALIDCALL;
    }

    CopyMasked(state_, block->second.state, block->second.mask);
    return D3D_OK;
}

HRESULT NullDevice::DeleteStateBlock(DWORD token)
{
    if (stateBlocks_.erase(token) == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    return D3D_OK;
}

HRESULT NullDevice::CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token)
{
    if (token == nullptr || recording_)
    {
        return D3DERR_INVALIDCALL;
    }

    if (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE)
    {
        return D3DERR_INVALIDCALL;
    }

    *token      = nextStateBlock_++;
    auto& block = stateBlocks_[*token];
    block.state = state_;
    block.mask  = MaskFor(type, state_);
    return D3D_OK;
}

HRESULT NullDevice::GetTexture(DWORD stage, IDirect3DBaseTexture8** texture)
{
    if (texture == nullptr || stage >= kTextureStages)
    {
        return D3DERR_INVALIDCALL;
    }

    *texture = state_.textures[stage].Share();
    return D3D_OK;
}

HRESULT NullDevice::SetTexture(DWORD stage, IDirect3DBaseTexture8* texture)
{
    if (stage >= kTextureStages)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().textures[stage].Reset(texture);
    if (auto* const mask = Recorded())
    {
        mask->textures.set(stage);
    }

    return D3D_OK;
}

HRESULT NullDevice::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value)
{
    if (value == nullptr || stage >= kTextureStages || static_cast<DWORD>(type) >= kTextureStageStates)
    {
        return D3DERR_INVALIDCALL;
    }

    *value = state_.textureStates[stage][type];
    return D3D_OK;
}

HRESULT NullDevice::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (stage >= kTextureStages || static_cast<DWORD>(type) >= kTextureStageStates)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().textureStates[stage][type] = value;
    if (auto* const mask = Recorded())
    {
        mask->textureStates.set(stage * kTextureStageStates + type);
    }

    return D3D_OK;
}

HRESULT NullDevice::ValidateDevice(DWORD* passes)
{
    return Store(passes, 1, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::SetPaletteEntries(UINT palette, const PALETTEENTRY* entries)
{
    if (entries == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    std::copy(entries, entries + 256, palettes_[palette].begin());
    return D3D_OK;
}

HRESULT NullDevice::GetPaletteEntries(UINT palette, PALETTEENTRY* entries)
{
    const auto found = palettes_.find(palette);
    if (entries == nullptr || found == palettes_.end())
    {
        return D3DERR_INVALIDCALL;
    }

    std::copy(found->second.begin(), found->second.end(), entries);
    return D3D_OK;
}

HRESULT NullDevice::SetCurrentTexturePalette(UINT palette)
{
    Target().texturePalette = palette;
    if (auto* const mask = Recorded())
    {
        mask->texturePalette = true;
    }

    return D3D_OK;
}

HRESULT NullDevice::GetCurrentTexturePalette(UINT* palette)
{
    return Store(palette, state_.texturePalette, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::SetStreamSource(UINT index, IDirect3DVertexBuffer8* buffer, UINT stride)
{
    if (index >= kStreams)
    {
        return D3DERR_INVALIDCALL;
    }

    auto& stream = Target().streams[index];
    stream.buffer.Reset(buffer);
    stream.stride = stride;
    if (auto* const mask = Recorded())
    {
        mask->streams.set(index);
    }

    return D3D_OK;
}

HRESULT NullDevice::GetStreamSource(UINT index, IDirect3DVertexBuffer8** buffer, UINT* stride)
{
    if (buffer == nullptr || stride == nullptr || index >= kStreams)
    {
        return D3DERR_INVALIDCALL;
    }

    *buffer = state_.streams[index].buffer.Share();
    *stride = state_.streams[index].stride;
    return D3D_OK;
}

HRESULT NullDevice::SetIndices(IDirect3DIndexBuffer8* buffer, UINT baseVertexIndex)
{
    Target().indices.Reset(buffer);
    Target().baseVertexIndex = baseVertexIndex;
    if (auto* const mask = Recorded())
    {
        mask->indices = true;
    }

    return D3D_OK;
}

HRESULT NullDevice::GetIndices(IDirect3DIndexBuffer8** buffer, UINT* baseVertexIndex)
{
    if (buffer == nullptr || baseVertexIndex == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *buffer          = state_.indices.Share();
    *baseVertexIndex = state_.baseVertexIndex;
    return D3D_OK;
}

HRESULT NullDevice::Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
{
    gl::Present(*this);
    return D3D_OK;
}

HRESULT NullDevice::BeginScene()
{
    gl::BeginScene();
    return D3D_OK;
}

HRESULT NullDevice::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    gl::Clear(*this, count, rects, flags, color, z, stencil);
    return D3D_OK;
}

HRESULT NullDevice::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    gl::Draw(*this, { type, count, start });
    return D3D_OK;
}

HRESULT NullDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT start, UINT count)
{
    gl::Draw(*this, { type, count, start, true });
    return D3D_OK;
}

HRESULT NullDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride)
{
    if (vertices == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    gl::Draw(*this, { type, count, 0, false, vertices, stride });
    return D3D_OK;
}

} // namespace nulldx
