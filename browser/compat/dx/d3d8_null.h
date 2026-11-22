// The Direct3D 8 device keeps what the client gives it in CPU memory; gl/ draws it with a canvas, nothing under Node.
#pragma once

#include "com.h"
#include "d3d8_formats.h"

#include "../gl/object.h"

#include <array>
#include <bitset>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace nulldx
{

// An owning COM pointer, so device state and state blocks hold the resources bound in them the way D3D8 does.
template <class T>
class Ref
{
public:
    Ref() = default;

    explicit Ref(T* p)
    : p_(p)
    {
        if (p_ != nullptr)
        {
            p_->AddRef();
        }
    }

    Ref(const Ref& other)
    : Ref(other.p_)
    {
    }

    Ref(Ref&& other) noexcept
    : p_(std::exchange(other.p_, nullptr))
    {
    }

    auto operator=(const Ref& other) -> Ref&
    {
        Reset(other.p_);
        return *this;
    }

    auto operator=(Ref&& other) noexcept -> Ref&
    {
        if (this != &other)
        {
            if (p_ != nullptr)
            {
                p_->Release();
            }

            p_ = std::exchange(other.p_, nullptr);
        }

        return *this;
    }

    ~Ref()
    {
        if (p_ != nullptr)
        {
            p_->Release();
        }
    }

    auto Reset(T* p = nullptr) -> void
    {
        if (p != nullptr)
        {
            p->AddRef();
        }

        if (p_ != nullptr)
        {
            p_->Release();
        }

        p_ = p;
    }

    auto Get() const noexcept -> T*
    {
        return p_;
    }

    auto operator->() const noexcept -> T*
    {
        return p_;
    }

    explicit operator bool() const noexcept
    {
        return p_ != nullptr;
    }

    // A new reference for the caller, as COM getters return.
    auto Share() const -> T*
    {
        if (p_ != nullptr)
        {
            p_->AddRef();
        }

        return p_;
    }

    // Takes over the reference a new object starts with.
    static auto Adopt(T* p) -> Ref
    {
        auto ref = Ref{};
        ref.p_   = p;
        return ref;
    }

private:
    T* p_ = nullptr;
};

class NullDevice;

// There is no window to size a windowed device, so the desktop gives the size and format.
auto CompletePresentParameters(D3DPRESENT_PARAMETERS& params) -> void;

// Holds no reference to the device, whose state holds references to its children: the two would keep each other alive.
template <class I>
class Child : public Object<I>
{
public:
    explicit Child(NullDevice* device)
    : device_(device)
    {
    }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) override;

    // The client keeps no private data on its resources.
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override
    {
        return D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override
    {
        return D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override
    {
        return D3DERR_NOTFOUND;
    }

    auto device() const -> NullDevice*
    {
        return device_;
    }

private:
    NullDevice* device_;
};

template <class I>
class Resource : public Child<I>
{
public:
    Resource(NullDevice* device, D3DRESOURCETYPE type)
    : Child<I>(device)
    , type_(type)
    {
    }

    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override
    {
        return std::exchange(priority_, priority);
    }

    DWORD STDMETHODCALLTYPE GetPriority() override
    {
        return priority_;
    }

    void STDMETHODCALLTYPE PreLoad() override
    {
    }

    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override
    {
        return type_;
    }

private:
    D3DRESOURCETYPE type_;
    DWORD           priority_ = 0;
};

// Surface memory is allocated on the first lock: render targets and depth buffers are never locked headless.
class NullSurface final : public Child<IDirect3DSurface8>
{
public:
    NullSurface(NullDevice* device, IUnknown* container, const D3DSURFACE_DESC& desc);

    ULONG STDMETHODCALLTYPE   AddRef() override;
    ULONG STDMETHODCALLTYPE   Release() override;
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;

    auto Desc() const -> const D3DSURFACE_DESC&
    {
        return desc_;
    }

    auto Pitch() const -> UINT
    {
        return RowPitch(desc_.Format, desc_.Width);
    }

    // Any caller may write through it, so it marks the texels for upload.
    auto Bits() -> BYTE*;

    auto Container() const -> IUnknown*
    {
        return container_;
    }

    // Only a surface of its own uses gl; a texture's level uses the texture's.
    gl::Name     gl;
    bool         glDirty = true;
    gl::Readback readback;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3DSurface8;
    }

private:
    // Set when a texture owns the surface: references then count on the texture, as in D3D8.
    IUnknown*         container_;
    D3DSURFACE_DESC   desc_;
    std::vector<BYTE> bits_;
};

template <class I>
class BaseTexture : public Resource<I>
{
public:
    BaseTexture(NullDevice* device, D3DRESOURCETYPE type, DWORD levels)
    : Resource<I>(device, type)
    , levels_(levels)
    {
    }

    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override
    {
        return std::exchange(lod_, lod);
    }

    DWORD STDMETHODCALLTYPE GetLOD() override
    {
        return lod_;
    }

    DWORD STDMETHODCALLTYPE GetLevelCount() override
    {
        return levels_;
    }

private:
    DWORD levels_;
    DWORD lod_ = 0;
};

class NullTexture final : public BaseTexture<IDirect3DTexture8>
{
public:
    NullTexture(NullDevice* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;

    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* rect) override
    {
        return D3D_OK;
    }

    auto Level(UINT level) const -> NullSurface*
    {
        if (level >= levels_.size())
        {
            return nullptr;
        }

        return levels_[level].get();
    }

    gl::Name gl;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3DResource8 || riid == IID_IDirect3DBaseTexture8 || riid == IID_IDirect3DTexture8;
    }

private:
    std::vector<std::unique_ptr<NullSurface>> levels_;
};

class NullCubeTexture final : public BaseTexture<IDirect3DCubeTexture8>
{
public:
    NullCubeTexture(NullDevice* device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(
        D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(
        D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override;

    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) override
    {
        return D3D_OK;
    }

    auto Face(D3DCUBEMAP_FACES face, UINT level) const -> NullSurface*;

    gl::Name gl;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3DResource8 || riid == IID_IDirect3DBaseTexture8 || riid == IID_IDirect3DCubeTexture8;
    }

private:
    UINT levelCount_;
    // Face-major: face * levelCount_ + level.
    std::vector<std::unique_ptr<NullSurface>> faces_;
};

class NullVertexBuffer final : public Resource<IDirect3DVertexBuffer8>
{
public:
    NullVertexBuffer(NullDevice* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override;

    HRESULT STDMETHODCALLTYPE Unlock() override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* desc) override;

    auto Data() const -> const std::vector<BYTE>&
    {
        return data_;
    }

    gl::Name  gl;
    gl::Dirty glDirty;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3DResource8 || riid == IID_IDirect3DVertexBuffer8;
    }

private:
    D3DVERTEXBUFFER_DESC desc_;
    std::vector<BYTE>    data_;
};

class NullIndexBuffer final : public Resource<IDirect3DIndexBuffer8>
{
public:
    NullIndexBuffer(NullDevice* device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override;

    HRESULT STDMETHODCALLTYPE Unlock() override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* desc) override;

    auto Data() const -> const std::vector<BYTE>&
    {
        return data_;
    }

    auto Format() const -> D3DFORMAT
    {
        return desc_.Format;
    }

    gl::Name  gl;
    gl::Dirty glDirty;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3DResource8 || riid == IID_IDirect3DIndexBuffer8;
    }

private:
    D3DINDEXBUFFER_DESC desc_;
    std::vector<BYTE>   data_;
};

class NullDirect3D final : public Object<IDirect3D8>
{
public:
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* initialize) override
    {
        return D3D_OK;
    }

    UINT STDMETHODCALLTYPE GetAdapterCount() override
    {
        return 1;
    }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(
        UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier) override;
    UINT STDMETHODCALLTYPE    GetAdapterModeCount(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* displayMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* displayMode) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(
        UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat, D3DFORMAT backBufferFormat, BOOL windowed) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage, D3DRESOURCETYPE resourceType, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
        UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE multiSample) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps) override;

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override
    {
        return nullptr;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice8** device) override;

    static auto DesktopMode() -> const D3DDISPLAYMODE&;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3D8;
    }
};

struct Light
{
    D3DLIGHT8 light;
    BOOL      enabled;
};

struct Stream
{
    Ref<IDirect3DVertexBuffer8> buffer;
    UINT                        stride = 0;
};

constexpr auto kRenderStates       = DWORD{ 256 };
constexpr auto kTextureStages      = DWORD{ 8 };
constexpr auto kTextureStageStates = DWORD{ 32 };
constexpr auto kTransforms         = DWORD{ 512 };
constexpr auto kVertexConstants    = DWORD{ 96 };
constexpr auto kPixelConstants     = DWORD{ 8 };
constexpr auto kStreams            = DWORD{ 16 };

struct DeviceState
{
    std::array<DWORD, kRenderStates>                                   renderStates{};
    std::array<std::array<DWORD, kTextureStageStates>, kTextureStages> textureStates{};
    std::array<Ref<IDirect3DBaseTexture8>, kTextureStages>             textures;
    std::array<D3DMATRIX, kTransforms>                                 transforms{};
    D3DMATERIAL8                                                       material{};
    std::map<DWORD, Light>                                             lights;
    D3DVIEWPORT8                                                       viewport{};
    DWORD                                                              vertexShader = 0;
    DWORD                                                              pixelShader  = 0;
    std::array<std::array<float, 4>, kVertexConstants>                 vertexConstants{};
    std::array<std::array<float, 4>, kPixelConstants>                  pixelConstants{};
    std::array<Stream, kStreams>                                       streams;
    Ref<IDirect3DIndexBuffer8>                                         indices;
    UINT                                                               baseVertexIndex = 0;
    UINT                                                               texturePalette  = 0;
};

struct StateMask
{
    std::bitset<kRenderStates>                        renderStates;
    std::bitset<kTextureStages * kTextureStageStates> textureStates;
    std::bitset<kTextureStages>                       textures;
    std::bitset<kTransforms>                          transforms;
    std::set<DWORD>                                   lights;
    std::bitset<kVertexConstants>                     vertexConstants;
    std::bitset<kPixelConstants>                      pixelConstants;
    std::bitset<kStreams>                             streams;
    bool                                              material       = false;
    bool                                              viewport       = false;
    bool                                              vertexShader   = false;
    bool                                              pixelShader    = false;
    bool                                              indices        = false;
    bool                                              texturePalette = false;
};

struct StateBlock
{
    DeviceState state;
    StateMask   mask;
};

auto DefaultState(const D3DPRESENT_PARAMETERS& params) -> DeviceState;
auto CopyMasked(const DeviceState& from, DeviceState& to, const StateMask& mask) -> void;
auto MaskFor(D3DSTATEBLOCKTYPE type, const DeviceState& state) -> StateMask;

class NullDevice final : public Object<IDirect3DDevice8>
{
public:
    NullDevice(
        NullDirect3D* direct3D, const D3DDEVICE_CREATION_PARAMETERS& creation, const D3DPRESENT_PARAMETERS& params);

    NullDevice(const NullDevice&) = delete;

    NullDevice(NullDevice&&) = delete;

    auto operator=(const NullDevice&) -> NullDevice& = delete;

    auto operator=(NullDevice&&) -> NullDevice& = delete;

    ~NullDevice() override;

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override
    {
        return D3D_OK;
    }

    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override
    {
        return 512u << 20;
    }

    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD bytes) override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** direct3D) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8* caps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override;

    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap) override
    {
        return D3D_OK;
    }

    void STDMETHODCALLTYPE SetCursorPosition(UINT x, UINT y, DWORD flags) override
    {
    }

    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override
    {
        return std::exchange(cursorVisible_, show);
    }

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8**) override
    {
        return D3DERR_NOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* params) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* status) override;

    void STDMETHODCALLTYPE SetGammaRamp(DWORD, const D3DGAMMARAMP*) override
    {
    }

    void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP*) override
    {
    }

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture) override;

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(
        UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture8**) override
    {
        return D3DERR_NOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(
        UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8** texture) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(
        UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** buffer) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
        UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** buffer) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, BOOL lockable, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE CreateImageSurface(
        UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8* source, const RECT* rects, UINT count, IDirect3DSurface8* destination, const POINT* points) override;

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination) override;

    HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8* destination) override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE SetRenderTarget(
        IDirect3DSurface8* renderTarget, IDirect3DSurface8* depthStencil) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8** renderTarget) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8** depthStencil) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;

    HRESULT STDMETHODCALLTYPE EndScene() override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE Clear(
        DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE type, D3DMATRIX* matrix) override;

    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8* viewport) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8* viewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8* material) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8* material) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT8* light) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT8* light) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;

    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, BOOL*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD, const float*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD, float*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* token) override;
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) override;

    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture8** texture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override;

    HRESULT STDMETHODCALLTYPE GetInfo(DWORD id, void* info, DWORD size) override
    {
        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* entries) override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override;
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* palette) override;

    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
        D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT start, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
        D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride) override;

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
        D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE ProcessVertices(
        UINT sourceStart, UINT destinationIndex, UINT count, IDirect3DVertexBuffer8* destination, DWORD flags) override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVertexShader(
        const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD usage) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD index, const void* data, DWORD count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD index, void* data, DWORD count) override;

    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD, void*, DWORD*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD, void*, DWORD*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT index, IDirect3DVertexBuffer8* buffer, UINT stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT index, IDirect3DVertexBuffer8** buffer, UINT* stride) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8* buffer, UINT baseVertexIndex) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8** buffer, UINT* baseVertexIndex) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* function, DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD index, const void* data, DWORD count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD index, void* data, DWORD count) override;

    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD, void*, DWORD*) override
    {
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float* segments, const D3DRECTPATCH_INFO* info) override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float* segments, const D3DTRIPATCH_INFO* info) override
    {
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE DeletePatch(UINT handle) override
    {
        return D3D_OK;
    }

    struct Shader
    {
        std::vector<DWORD> declaration;
        std::vector<DWORD> function;
    };

    auto State() const -> const DeviceState&
    {
        return state_;
    }

    auto RenderTarget() const -> NullSurface*
    {
        return static_cast<NullSurface*>(renderTarget_.Get());
    }

    auto DepthStencil() const -> NullSurface*
    {
        return static_cast<NullSurface*>(depthStencil_.Get());
    }

    auto BackBuffer() const -> NullSurface*
    {
        return backBuffer_.Get();
    }

    // nullptr for a handle the device never made.
    auto VertexShader(DWORD handle) const -> const Shader*;
    auto PixelShader(DWORD handle) const -> const Shader*;

protected:
    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirect3DDevice8;
    }

private:
    auto CreateImplicitSurfaces() -> void;

    // While a state block records, Set calls go to it instead of the device, as in D3D8.
    auto Target() -> DeviceState&
    {
        if (recording_)
        {
            return recording_->state;
        }

        return state_;
    }

    auto Recorded() -> StateMask*
    {
        if (recording_)
        {
            return &recording_->mask;
        }

        return nullptr;
    }

    NullDirect3D*                                 direct3D_;
    D3DDEVICE_CREATION_PARAMETERS                 creation_;
    D3DPRESENT_PARAMETERS                         params_;
    Ref<NullSurface>                              backBuffer_;
    Ref<NullSurface>                              autoDepthStencil_;
    Ref<IDirect3DSurface8>                        renderTarget_;
    Ref<IDirect3DSurface8>                        depthStencil_;
    DeviceState                                   state_;
    std::unique_ptr<StateBlock>                   recording_;
    std::map<DWORD, StateBlock>                   stateBlocks_;
    DWORD                                         nextStateBlock_ = 1;
    std::map<DWORD, Shader>                       vertexShaders_;
    std::map<DWORD, Shader>                       pixelShaders_;
    DWORD                                         nextShader_ = 1;
    std::map<UINT, std::array<PALETTEENTRY, 256>> palettes_;
    BOOL                                          cursorVisible_ = FALSE;
};

template <class I>
HRESULT Child<I>::GetDevice(IDirect3DDevice8** device)
{
    if (device == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    device_->AddRef();
    *device = device_;
    return D3D_OK;
}

} // namespace nulldx
