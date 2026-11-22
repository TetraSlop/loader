#pragma once
#include <windows.h>

#include <unknwn.h>

#include "app/com.h"

// Direct3D 8 ABI, declared independently so the obsolete SDK is not required.
namespace tmloader::d3d8
{

// IDirect3D8 vtable slots.
enum Direct3D : unsigned
{
    GetAdapterDisplayMode = 8,
    CreateDevice          = 15,
    Direct3DMethodCount   = 16,
};

// IDirect3DDevice8 vtable slots.
enum Device : unsigned
{
    Reset                   = 14,
    Present                 = 15,
    GetBackBuffer           = 16,
    CreateTexture           = 20,
    CreateVertexBuffer      = 23,
    BeginScene              = 34,
    EndScene                = 35,
    Clear                   = 36,
    SetRenderState          = 50,
    SetTexture              = 61,
    SetTextureStageState    = 63,
    DrawPrimitive           = 70,
    CreateVertexShader      = 75,
    SetVertexShader         = 76,
    DeleteVertexShader      = 78,
    SetVertexShaderConstant = 79,
    SetStreamSource         = 83,
    DeviceMethodCount       = 97,
};

// IDirect3DTexture8 / IDirect3DSurface8 / IDirect3DVertexBuffer8 slots.
enum Resource : unsigned
{
    SurfaceGetDesc  = 8,
    SurfaceLockRect = 9,
    SurfaceUnlock   = 10,
    BufferLock      = 11,
    BufferUnlock    = 12,
    TextureLockRect = 16,
    TextureUnlock   = 17,
};

// D3DFORMAT
enum Format : DWORD
{
    FormatA8R8G8B8 = 21,
    FormatX8R8G8B8 = 22,
};

// D3DPOOL
enum Pool : DWORD
{
    PoolManaged = 1,
};

// D3DSWAPEFFECT
enum SwapEffect : DWORD
{
    SwapEffectCopy = 3,
};

// D3DPRIMITIVETYPE
enum PrimitiveType : DWORD
{
    TriangleStrip = 5,
};

// D3DDEVTYPE
enum DeviceType : DWORD
{
    DeviceTypeHal = 1,
};

// D3DLOCK_*
enum LockFlag : DWORD
{
    LockReadOnly = 0x10,
    LockDiscard  = 0x2000,
};

// The SDK defines these as plain flags.
constexpr DWORD kSoftwareVertexProcessing = 0x20; // D3DCREATE_SOFTWARE_VERTEXPROCESSING
constexpr DWORD kLockableBackBuffer       = 1;    // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER

struct PresentParameters
{
    DWORD width;
    DWORD height;
    DWORD format;
    DWORD count;
    DWORD multisample;
    DWORD swap;
    HWND  window;
    BOOL  windowed;
    BOOL  depth;
    DWORD depthFormat;
    DWORD flags;
    DWORD refresh;
    DWORD interval;
};

struct DisplayMode
{
    DWORD width;
    DWORD height;
    DWORD refresh;
    DWORD format;
};

struct SurfaceDesc
{
    DWORD format;
    DWORD type;
    DWORD usage;
    DWORD pool;
    DWORD size;
    DWORD multisample;
    DWORD width;
    DWORD height;

    auto operator==(const SurfaceDesc&) const -> bool = default;
};

struct LockedRect
{
    INT   pitch;
    BYTE* bits;
};

static_assert(sizeof(PresentParameters) == 52, "D3D8 presentation parameters require x86");
static_assert(sizeof(SurfaceDesc) == 32);

} // namespace tmloader::d3d8
