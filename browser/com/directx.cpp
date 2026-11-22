#include "com/directx.h"

#include "com/d3dx.h"

#include "com/proxies.h"

#include <d3d8.h>
#include <dinput.h>
#include <windows.h>

#include <mmsystem.h>

#include <dsound.h>

#include "dx/d3d8_formats.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

template <>
struct tmweb::Handle<HWND__> : std::true_type
{
};

template <>
struct tmweb::Handle<HINSTANCE__> : std::true_type
{
};

template <>
struct tmweb::Handle<HMONITOR__> : std::true_type
{
};

namespace tmweb
{
namespace
{

auto iid(const GUID& guid) -> Iid
{
    return std::bit_cast<Iid>(guid);
}

auto code(const HRESULT result) -> Return
{
    return Return{ static_cast<std::uint32_t>(result) };
}

template <class T>
auto pointer(Proxies& proxies, const std::uint32_t raw) -> T*
{
    return detail::Value<T*>::translate(proxies, raw);
}

// A hand-written slot; body takes the Call and returns its Outcome
template <class F>
auto method(std::string name, const std::uint8_t argc, F body) -> Method
{
    return Method{ std::move(name), argc, [body](Call& call) -> Outcome
                   {
                       return guarded(call, [&]() -> Outcome
                                      {
                                          return body(call);
                                      });
                   } };
}

// A method TM has not been seen to call that needs more than a binding: halt and say which
auto unsupported(std::string name, const std::uint8_t argc) -> Method
{
    return Method{ std::move(name), argc, [](Call& call) -> Outcome
                   {
                       return Halt{ std::string{ call.name() } + " is not implemented" };
                   } };
}

// Direct3D sets the calling thread's x87 to single precision, round to nearest, exceptions masked, unless asked to
// preserve it. TM creates its device with behaviour 0x20, so its rules run at 24-bit precision from then on.
auto createDevice(Proxies& proxies) -> Method
{
    auto created    = TMWEB_BIND(proxies, IDirect3D8, CreateDevice);
    created.handler = [inner = std::move(created.handler)](Call& call) -> Outcome
    {
        auto        outcome = inner(call);
        const auto* result  = std::get_if<Return>(&outcome);
        if (result != nullptr && SUCCEEDED(static_cast<HRESULT>(result->eax)) && (call.arg(4) & D3DCREATE_FPU_PRESERVE) == 0)
        {
            auto& control = call.context().fpu.control;
            control       = static_cast<std::uint16_t>((control & ~0x0F00) | 0x003F);
        }
        return outcome;
    };
    return created;
}

template <class B>
auto bufferLock(Proxies& proxies) -> Method
{
    return method("Lock", 5, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self   = proxies.self<B>(call);
                      const auto  offset = call.arg(1);
                      const auto  size   = call.arg(2);
                      const auto  out    = call.arg(3);
                      if (out == 0)
                      {
                          return code(D3DERR_INVALIDCALL);
                      }
                      proxies.memory().check(out, 4, Access::Write);
                      auto*      bytes  = static_cast<BYTE*>(nullptr);
                      const auto result = self->Lock(offset, size, &bytes, call.arg(4));
                      if (FAILED(result))
                      {
                          return code(result);
                      }
                      auto desc = std::remove_pointer_t<std::tuple_element_t<0, typename detail::MemberTraits<decltype(&B::GetDesc)>::Args>>{};
                      self->GetDesc(&desc);
                      const auto rest   = offset < desc.Size ? desc.Size - offset : 0;
                      const auto length = size == 0 ? rest : std::min(size, rest);
                      proxies.memory().write<std::uint32_t>(out, proxies.lock(self, 0, bytes, length));
                      return code(result);
                  });
}

template <class B>
auto bufferUnlock(Proxies& proxies) -> Method
{
    return method("Unlock", 1, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<B>(call);
                      proxies.unlock(self, 0);
                      return code(self->Unlock());
                  });
}

// Stage what a LockRect locked, rect or the whole surface desc describes, and give the guest its D3DLOCKED_RECT
void stageRect(Proxies& proxies, const void* const host, const std::uint32_t sub, const D3DSURFACE_DESC& desc, const RECT* const rect, const D3DLOCKED_RECT& locked, const std::uint32_t out)
{
    const auto width  = rect == nullptr ? desc.Width : static_cast<UINT>(rect->right - rect->left);
    const auto height = rect == nullptr ? desc.Height : static_cast<UINT>(rect->bottom - rect->top);
    const auto rows   = nulldx::BlockRows(desc.Format, height);
    const auto size   = rows == 0 ? 0 : (rows - 1) * static_cast<UINT>(locked.Pitch) + nulldx::RowPitch(desc.Format, width);
    const auto guest  = proxies.lock(host, sub, static_cast<std::uint8_t*>(locked.pBits), size);
    proxies.memory().write<std::int32_t>(out, locked.Pitch);
    proxies.memory().write<std::uint32_t>(out + 4, guest);
}

// The D3DLOCKED_RECT out-pointer: false for null
auto lockedRect(Proxies& proxies, const std::uint32_t out) -> bool
{
    if (out == 0)
    {
        return false;
    }
    proxies.memory().check(out, 8, Access::Write);
    return true;
}

auto surfaceLockRect(Proxies& proxies) -> Method
{
    return method("LockRect", 4, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirect3DSurface8>(call);
                      const auto  out  = call.arg(1);
                      if (!lockedRect(proxies, out))
                      {
                          return code(D3DERR_INVALIDCALL);
                      }
                      const auto* const rect   = pointer<const RECT>(proxies, call.arg(2));
                      auto              locked = D3DLOCKED_RECT{};
                      const auto        result = self->LockRect(&locked, rect, call.arg(3));
                      if (FAILED(result))
                      {
                          return code(result);
                      }
                      auto desc = D3DSURFACE_DESC{};
                      self->GetDesc(&desc);
                      stageRect(proxies, self, 0, desc, rect, locked, out);
                      return code(result);
                  });
}

auto surfaceUnlockRect(Proxies& proxies) -> Method
{
    return method("UnlockRect", 1, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirect3DSurface8>(call);
                      proxies.unlock(self, 0);
                      return code(self->UnlockRect());
                  });
}

auto textureLockRect(Proxies& proxies) -> Method
{
    return method("LockRect", 5, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self  = proxies.self<IDirect3DTexture8>(call);
                      const auto  level = call.arg(1);
                      const auto  out   = call.arg(2);
                      if (!lockedRect(proxies, out))
                      {
                          return code(D3DERR_INVALIDCALL);
                      }
                      const auto* const rect   = pointer<const RECT>(proxies, call.arg(3));
                      auto              locked = D3DLOCKED_RECT{};
                      const auto        result = self->LockRect(level, &locked, rect, call.arg(4));
                      if (FAILED(result))
                      {
                          return code(result);
                      }
                      auto desc = D3DSURFACE_DESC{};
                      self->GetLevelDesc(level, &desc);
                      stageRect(proxies, self, level, desc, rect, locked, out);
                      return code(result);
                  });
}

auto textureUnlockRect(Proxies& proxies) -> Method
{
    return method("UnlockRect", 2, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirect3DTexture8>(call);
                      proxies.unlock(self, call.arg(1));
                      return code(self->UnlockRect(call.arg(1)));
                  });
}

// A cube texture's parts for staging: 32 levels per face
auto cubePart(const std::uint32_t face, const std::uint32_t level) -> std::uint32_t
{
    return face * 32 + level;
}

auto cubeLockRect(Proxies& proxies) -> Method
{
    return method("LockRect", 6, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self  = proxies.self<IDirect3DCubeTexture8>(call);
                      const auto  face  = call.arg(1);
                      const auto  level = call.arg(2);
                      const auto  out   = call.arg(3);
                      if (!lockedRect(proxies, out))
                      {
                          return code(D3DERR_INVALIDCALL);
                      }
                      const auto* const rect   = pointer<const RECT>(proxies, call.arg(4));
                      auto              locked = D3DLOCKED_RECT{};
                      const auto        result = self->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &locked, rect, call.arg(5));
                      if (FAILED(result))
                      {
                          return code(result);
                      }
                      auto desc = D3DSURFACE_DESC{};
                      self->GetLevelDesc(level, &desc);
                      stageRect(proxies, self, cubePart(face, level), desc, rect, locked, out);
                      return code(result);
                  });
}

auto cubeUnlockRect(Proxies& proxies) -> Method
{
    return method("UnlockRect", 3, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirect3DCubeTexture8>(call);
                      proxies.unlock(self, cubePart(call.arg(1), call.arg(2)));
                      return code(self->UnlockRect(static_cast<D3DCUBEMAP_FACES>(call.arg(1)), call.arg(2)));
                  });
}

// A QueryInterface-like out-pointer: the host's object for riid becomes that interface's proxy
template <class F>
auto queried(Proxies& proxies, const std::uint32_t riid, const std::uint32_t out, F query) -> HRESULT
{
    proxies.memory().write<std::uint32_t>(out, 0);
    auto iidBytes = Iid{};
    proxies.memory().read(riid, iidBytes);
    auto*      host   = static_cast<void*>(nullptr);
    const auto result = query(*pointer<const IID>(proxies, riid), &host);
    if (FAILED(result))
    {
        return result;
    }
    const auto guest = proxies.wrap(iidBytes, host);
    if (guest == 0)
    {
        static_cast<IUnknown*>(host)->Release();
        return E_NOINTERFACE;
    }
    proxies.memory().write<std::uint32_t>(out, guest);
    return result;
}

template <class S>
auto getContainer(Proxies& proxies) -> Method
{
    return method("GetContainer", 3, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<S>(call);
                      if (call.arg(2) == 0)
                      {
                          return code(D3DERR_INVALIDCALL);
                      }
                      return code(queried(proxies, call.arg(1), call.arg(2), [self](const IID& riid, void** host)
                                          {
                                              return self->GetContainer(riid, host);
                                          }));
                  });
}

// An enumeration's callback into guest code; each item is copied to a guest buffer first
template <class Item>
struct GuestCallback
{
    // Apart from the parts locks use
    static constexpr std::uint32_t kItemPart = 0x10000;

    Proxies*      proxies  = nullptr;
    std::uint32_t function = 0;
    std::uint32_t context  = 0;
    std::uint32_t item     = 0;

    GuestCallback(Proxies& owner, const void* const host, const std::uint32_t guestFunction, const std::uint32_t guestContext)
    : proxies(&owner)
    , function(guestFunction)
    , context(guestContext)
    , item(owner.staging(host, kItemPart, sizeof(Item)))
    {
    }

    static BOOL CALLBACK call(const Item* const host, LPVOID const self)
    {
        const auto& callback = *static_cast<GuestCallback*>(self);
        std::memcpy(callback.proxies->memory().host(callback.item), host, sizeof(Item));
        const auto args = std::array{ callback.item, callback.context };
        return static_cast<BOOL>(callback.proxies->machine().callGuest(callback.function, args, Convention::Stdcall));
    }
};

auto enumDevices(Proxies& proxies) -> Method
{
    return method("EnumDevices", 5, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirectInput8A>(call);
                      if (call.arg(2) == 0)
                      {
                          return code(DIERR_INVALIDPARAM);
                      }
                      auto callback = GuestCallback<DIDEVICEINSTANCEA>{ proxies, self, call.arg(2), call.arg(3) };
                      return code(self->EnumDevices(call.arg(1), &GuestCallback<DIDEVICEINSTANCEA>::call, &callback, call.arg(4)));
                  });
}

auto enumObjects(Proxies& proxies) -> Method
{
    return method("EnumObjects", 4, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirectInputDevice8A>(call);
                      if (call.arg(1) == 0)
                      {
                          return code(DIERR_INVALIDPARAM);
                      }
                      auto callback = GuestCallback<DIDEVICEOBJECTINSTANCEA>{ proxies, self, call.arg(1), call.arg(2) };
                      return code(self->EnumObjects(&GuestCallback<DIDEVICEOBJECTINSTANCEA>::call, &callback, call.arg(3)));
                  });
}

auto enumEffects(Proxies& proxies) -> Method
{
    return method("EnumEffects", 4, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self = proxies.self<IDirectInputDevice8A>(call);
                      if (call.arg(1) == 0)
                      {
                          return code(DIERR_INVALIDPARAM);
                      }
                      auto callback = GuestCallback<DIEFFECTINFOA>{ proxies, self, call.arg(1), call.arg(2) };
                      return code(self->EnumEffects(&GuestCallback<DIEFFECTINFOA>::call, &callback, call.arg(3)));
                  });
}

// DirectSound locks up to two regions, when the range wraps around the buffer's end
template <class B>
auto soundLock(Proxies& proxies) -> Method
{
    return method("Lock", 8, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self        = proxies.self<B>(call);
                      auto&       memory      = proxies.memory();
                      const auto  firstOut    = call.arg(3);
                      const auto  firstSize   = call.arg(4);
                      const auto  secondOut   = call.arg(5);
                      const auto  secondSize  = call.arg(6);
                      auto*       first       = static_cast<void*>(nullptr);
                      auto*       second      = static_cast<void*>(nullptr);
                      auto        firstBytes  = DWORD{ 0 };
                      auto        secondBytes = DWORD{ 0 };
                      for (const auto out : { firstOut, firstSize, secondOut, secondSize })
                      {
                          if (out != 0)
                          {
                              memory.check(out, 4, Access::Write);
                          }
                      }
                      const auto result = self->Lock(call.arg(1), call.arg(2), firstOut != 0 ? &first : nullptr, firstSize != 0 ? &firstBytes : nullptr, secondOut != 0 ? &second : nullptr, secondSize != 0 ? &secondBytes : nullptr, call.arg(7));
                      if (FAILED(result))
                      {
                          return code(result);
                      }
                      const auto write = [&memory](const std::uint32_t out, const std::uint32_t value)
                      {
                          if (out != 0)
                          {
                              memory.write<std::uint32_t>(out, value);
                          }
                      };
                      write(firstOut, first == nullptr ? 0 : proxies.lock(self, 0, static_cast<std::uint8_t*>(first), firstBytes));
                      write(firstSize, firstBytes);
                      write(secondOut, second == nullptr ? 0 : proxies.lock(self, 1, static_cast<std::uint8_t*>(second), secondBytes));
                      write(secondSize, secondBytes);
                      return code(result);
                  });
}

template <class B>
auto soundUnlock(Proxies& proxies) -> Method
{
    return method("Unlock", 5, [&proxies](Call& call) -> Outcome
                  {
                      auto* const self   = proxies.self<B>(call);
                      auto* const first  = proxies.unlock(self, 0);
                      auto* const second = proxies.unlock(self, 1);
                      return code(self->Unlock(first, call.arg(2), second, call.arg(4)));
                  });
}

// Every slot after IUnknown's, in vtable order, as mingw-w64's headers declare them
void addInterfaces(Proxies& proxies)
{
    proxies.add<IDirect3D8>("IDirect3D8", iid(IID_IDirect3D8), {
                                                                   TMWEB_BIND(proxies, IDirect3D8, RegisterSoftwareDevice),
                                                                   TMWEB_BIND(proxies, IDirect3D8, GetAdapterCount),
                                                                   TMWEB_BIND(proxies, IDirect3D8, GetAdapterIdentifier),
                                                                   TMWEB_BIND(proxies, IDirect3D8, GetAdapterModeCount),
                                                                   TMWEB_BIND(proxies, IDirect3D8, EnumAdapterModes),
                                                                   TMWEB_BIND(proxies, IDirect3D8, GetAdapterDisplayMode),
                                                                   TMWEB_BIND(proxies, IDirect3D8, CheckDeviceType),
                                                                   TMWEB_BIND(proxies, IDirect3D8, CheckDeviceFormat),
                                                                   TMWEB_BIND(proxies, IDirect3D8, CheckDeviceMultiSampleType),
                                                                   TMWEB_BIND(proxies, IDirect3D8, CheckDepthStencilMatch),
                                                                   TMWEB_BIND(proxies, IDirect3D8, GetDeviceCaps),
                                                                   TMWEB_BIND(proxies, IDirect3D8, GetAdapterMonitor),
                                                                   createDevice(proxies),
                                                               });

    proxies.add<IDirect3DDevice8>("IDirect3DDevice8", iid(IID_IDirect3DDevice8), {
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, TestCooperativeLevel),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetAvailableTextureMem),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, ResourceManagerDiscardBytes),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetDirect3D),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetDeviceCaps),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetDisplayMode),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetCreationParameters),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetCursorProperties),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetCursorPosition),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, ShowCursor),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateAdditionalSwapChain),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, Reset),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, Present),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetBackBuffer),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetRasterStatus),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetGammaRamp),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetGammaRamp),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateTexture),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateVolumeTexture),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateCubeTexture),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateVertexBuffer),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateIndexBuffer),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateRenderTarget),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateDepthStencilSurface),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateImageSurface),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CopyRects),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, UpdateTexture),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetFrontBuffer),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetRenderTarget),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetRenderTarget),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetDepthStencilSurface),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, BeginScene),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, EndScene),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, Clear),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetTransform),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetTransform),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, MultiplyTransform),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetViewport),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetViewport),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetMaterial),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetMaterial),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetLight),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetLight),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, LightEnable),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetLightEnable),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetClipPlane),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetClipPlane),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetRenderState),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetRenderState),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, BeginStateBlock),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, EndStateBlock),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, ApplyStateBlock),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CaptureStateBlock),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DeleteStateBlock),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateStateBlock),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetClipStatus),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetClipStatus),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetTexture),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetTexture),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetTextureStageState),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetTextureStageState),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, ValidateDevice),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetInfo),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetPaletteEntries),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetPaletteEntries),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetCurrentTexturePalette),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetCurrentTexturePalette),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DrawPrimitive),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DrawIndexedPrimitive),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DrawPrimitiveUP),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DrawIndexedPrimitiveUP),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, ProcessVertices),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreateVertexShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetVertexShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetVertexShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DeleteVertexShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetVertexShaderConstant),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetVertexShaderConstant),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetVertexShaderDeclaration),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetVertexShaderFunction),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetStreamSource),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetStreamSource),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetIndices),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetIndices),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, CreatePixelShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetPixelShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetPixelShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DeletePixelShader),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, SetPixelShaderConstant),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetPixelShaderConstant),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, GetPixelShaderFunction),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DrawRectPatch),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DrawTriPatch),
                                                                                     TMWEB_BIND(proxies, IDirect3DDevice8, DeletePatch),
                                                                                 });

    proxies.add<IDirect3DSwapChain8>("IDirect3DSwapChain8", iid(IID_IDirect3DSwapChain8), {
                                                                                              TMWEB_BIND(proxies, IDirect3DSwapChain8, Present),
                                                                                              TMWEB_BIND(proxies, IDirect3DSwapChain8, GetBackBuffer),
                                                                                          });

    proxies.add<IDirect3DTexture8, IDirect3DBaseTexture8, IDirect3DResource8>("IDirect3DTexture8", iid(IID_IDirect3DTexture8), {
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetDevice),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, SetPrivateData),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetPrivateData),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, FreePrivateData),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, SetPriority),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetPriority),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, PreLoad),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetType),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, SetLOD),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetLOD),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetLevelCount),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetLevelDesc),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, GetSurfaceLevel),
                                                                                                                                   textureLockRect(proxies),
                                                                                                                                   textureUnlockRect(proxies),
                                                                                                                                   TMWEB_BIND(proxies, IDirect3DTexture8, AddDirtyRect),
                                                                                                                               });

    proxies.add<IDirect3DVolumeTexture8, IDirect3DBaseTexture8, IDirect3DResource8>("IDirect3DVolumeTexture8", iid(IID_IDirect3DVolumeTexture8), {
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetDevice),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, SetPrivateData),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetPrivateData),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, FreePrivateData),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, SetPriority),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetPriority),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, PreLoad),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetType),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, SetLOD),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetLOD),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetLevelCount),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetLevelDesc),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, GetVolumeLevel),
                                                                                                                                                     unsupported("LockBox", 5),
                                                                                                                                                     unsupported("UnlockBox", 2),
                                                                                                                                                     TMWEB_BIND(proxies, IDirect3DVolumeTexture8, AddDirtyBox),
                                                                                                                                                 });

    proxies.add<IDirect3DCubeTexture8, IDirect3DBaseTexture8, IDirect3DResource8>("IDirect3DCubeTexture8", iid(IID_IDirect3DCubeTexture8), {
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetDevice),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, SetPrivateData),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetPrivateData),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, FreePrivateData),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, SetPriority),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetPriority),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, PreLoad),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetType),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, SetLOD),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetLOD),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetLevelCount),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetLevelDesc),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, GetCubeMapSurface),
                                                                                                                                               cubeLockRect(proxies),
                                                                                                                                               cubeUnlockRect(proxies),
                                                                                                                                               TMWEB_BIND(proxies, IDirect3DCubeTexture8, AddDirtyRect),
                                                                                                                                           });

    proxies.add<IDirect3DVertexBuffer8, IDirect3DResource8>("IDirect3DVertexBuffer8", iid(IID_IDirect3DVertexBuffer8), {
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, GetDevice),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, SetPrivateData),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, GetPrivateData),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, FreePrivateData),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, SetPriority),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, GetPriority),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, PreLoad),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, GetType),
                                                                                                                           bufferLock<IDirect3DVertexBuffer8>(proxies),
                                                                                                                           bufferUnlock<IDirect3DVertexBuffer8>(proxies),
                                                                                                                           TMWEB_BIND(proxies, IDirect3DVertexBuffer8, GetDesc),
                                                                                                                       });

    proxies.add<IDirect3DIndexBuffer8, IDirect3DResource8>("IDirect3DIndexBuffer8", iid(IID_IDirect3DIndexBuffer8), {
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, GetDevice),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, SetPrivateData),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, GetPrivateData),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, FreePrivateData),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, SetPriority),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, GetPriority),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, PreLoad),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, GetType),
                                                                                                                        bufferLock<IDirect3DIndexBuffer8>(proxies),
                                                                                                                        bufferUnlock<IDirect3DIndexBuffer8>(proxies),
                                                                                                                        TMWEB_BIND(proxies, IDirect3DIndexBuffer8, GetDesc),
                                                                                                                    });

    proxies.add<IDirect3DSurface8>("IDirect3DSurface8", iid(IID_IDirect3DSurface8), {
                                                                                        TMWEB_BIND(proxies, IDirect3DSurface8, GetDevice),
                                                                                        TMWEB_BIND(proxies, IDirect3DSurface8, SetPrivateData),
                                                                                        TMWEB_BIND(proxies, IDirect3DSurface8, GetPrivateData),
                                                                                        TMWEB_BIND(proxies, IDirect3DSurface8, FreePrivateData),
                                                                                        getContainer<IDirect3DSurface8>(proxies),
                                                                                        TMWEB_BIND(proxies, IDirect3DSurface8, GetDesc),
                                                                                        surfaceLockRect(proxies),
                                                                                        surfaceUnlockRect(proxies),
                                                                                    });

    proxies.add<IDirect3DVolume8>("IDirect3DVolume8", iid(IID_IDirect3DVolume8), {
                                                                                     TMWEB_BIND(proxies, IDirect3DVolume8, GetDevice),
                                                                                     TMWEB_BIND(proxies, IDirect3DVolume8, SetPrivateData),
                                                                                     TMWEB_BIND(proxies, IDirect3DVolume8, GetPrivateData),
                                                                                     TMWEB_BIND(proxies, IDirect3DVolume8, FreePrivateData),
                                                                                     getContainer<IDirect3DVolume8>(proxies),
                                                                                     TMWEB_BIND(proxies, IDirect3DVolume8, GetDesc),
                                                                                     unsupported("LockBox", 4),
                                                                                     unsupported("UnlockBox", 1),
                                                                                 });

    proxies.add<IDirectInput8A>("IDirectInput8A", iid(IID_IDirectInput8A), {
                                                                               TMWEB_BIND(proxies, IDirectInput8A, CreateDevice),
                                                                               enumDevices(proxies),
                                                                               TMWEB_BIND(proxies, IDirectInput8A, GetDeviceStatus),
                                                                               TMWEB_BIND(proxies, IDirectInput8A, RunControlPanel),
                                                                               TMWEB_BIND(proxies, IDirectInput8A, Initialize),
                                                                               TMWEB_BIND(proxies, IDirectInput8A, FindDevice),
                                                                               unsupported("EnumDevicesBySemantics", 6),
                                                                               unsupported("ConfigureDevices", 5),
                                                                           });

    proxies.add<IDirectInputDevice8A>("IDirectInputDevice8A", iid(IID_IDirectInputDevice8A), {
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetCapabilities),
                                                                                                 enumObjects(proxies),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetProperty),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SetProperty),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, Acquire),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, Unacquire),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetDeviceState),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetDeviceData),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SetDataFormat),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SetEventNotification),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SetCooperativeLevel),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetObjectInfo),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetDeviceInfo),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, RunControlPanel),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, Initialize),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, CreateEffect),
                                                                                                 enumEffects(proxies),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetEffectInfo),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetForceFeedbackState),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SendForceFeedbackCommand),
                                                                                                 unsupported("EnumCreatedEffectObjects", 4),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, Escape),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, Poll),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SendDeviceData),
                                                                                                 unsupported("EnumEffectsInFile", 5),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, WriteEffectToFile),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, BuildActionMap),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, SetActionMap),
                                                                                                 TMWEB_BIND(proxies, IDirectInputDevice8A, GetImageInfo),
                                                                                             });

    proxies.add<IDirectSound8>("IDirectSound8", iid(IID_IDirectSound8), {
                                                                            TMWEB_BIND(proxies, IDirectSound8, CreateSoundBuffer),
                                                                            TMWEB_BIND(proxies, IDirectSound8, GetCaps),
                                                                            TMWEB_BIND(proxies, IDirectSound8, DuplicateSoundBuffer),
                                                                            TMWEB_BIND(proxies, IDirectSound8, SetCooperativeLevel),
                                                                            TMWEB_BIND(proxies, IDirectSound8, Compact),
                                                                            TMWEB_BIND(proxies, IDirectSound8, GetSpeakerConfig),
                                                                            TMWEB_BIND(proxies, IDirectSound8, SetSpeakerConfig),
                                                                            TMWEB_BIND(proxies, IDirectSound8, Initialize),
                                                                            TMWEB_BIND(proxies, IDirectSound8, VerifyCertification),
                                                                        });

    proxies.add<IDirectSoundBuffer>("IDirectSoundBuffer", iid(IID_IDirectSoundBuffer), {
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetCaps),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetCurrentPosition),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetFormat),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetVolume),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetPan),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetFrequency),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, GetStatus),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, Initialize),
                                                                                           soundLock<IDirectSoundBuffer>(proxies),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, Play),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, SetCurrentPosition),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, SetFormat),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, SetVolume),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, SetPan),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, SetFrequency),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, Stop),
                                                                                           soundUnlock<IDirectSoundBuffer>(proxies),
                                                                                           TMWEB_BIND(proxies, IDirectSoundBuffer, Restore),
                                                                                       });

    proxies.add<IDirectSoundBuffer8>("IDirectSoundBuffer8", iid(IID_IDirectSoundBuffer8), {
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetCaps),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetCurrentPosition),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetFormat),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetVolume),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetPan),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetFrequency),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, GetStatus),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, Initialize),
                                                                                              soundLock<IDirectSoundBuffer8>(proxies),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, Play),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, SetCurrentPosition),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, SetFormat),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, SetVolume),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, SetPan),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, SetFrequency),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, Stop),
                                                                                              soundUnlock<IDirectSoundBuffer8>(proxies),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, Restore),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, SetFX),
                                                                                              TMWEB_BIND(proxies, IDirectSoundBuffer8, AcquireResources),
                                                                                              unsupported("GetObjectInPath", 5),
                                                                                          });
}

} // namespace

auto addDirectX(Proxies& proxies) -> std::vector<Function>
{
    addInterfaces(proxies);

    auto functions = std::vector<Function>{};
    functions.push_back(Function{ "d3d8.dll", "Direct3DCreate8", Convention::Stdcall, 1, [&proxies](Call& call) -> Outcome
                                  {
                                      return Return{ proxies.wrap<IDirect3D8>(Direct3DCreate8(call.arg(0))) };
                                  } });

    functions.push_back(Function{ "dinput8.dll", "DirectInput8Create", Convention::Stdcall, 5, [&proxies](Call& call) -> Outcome
                                  {
                                      const auto out = call.arg(3);
                                      if (out == 0)
                                      {
                                          return code(DIERR_INVALIDPARAM);
                                      }
                                      if (call.arg(4) != 0)
                                      {
                                          proxies.memory().write<std::uint32_t>(out, 0);
                                          return code(DIERR_NOAGGREGATION);
                                      }
                                      const auto instance = reinterpret_cast<HINSTANCE>(static_cast<std::uintptr_t>(call.arg(0)));
                                      const auto version  = call.arg(1);
                                      return code(queried(proxies, call.arg(2), out, [instance, version](const IID& riid, void** host)
                                                          {
                                                              return DirectInput8Create(instance, version, riid, host, nullptr);
                                                          }));
                                  } });

    const auto directSound = [&proxies](Call& call) -> Outcome
    {
        const auto out = call.arg(1);
        if (out == 0)
        {
            return code(DSERR_INVALIDPARAM);
        }
        proxies.memory().check(out, 4, Access::Write);
        if (call.arg(2) != 0)
        {
            proxies.memory().write<std::uint32_t>(out, 0);
            return code(DSERR_NOAGGREGATION);
        }
        auto*      host   = static_cast<IDirectSound8*>(nullptr);
        const auto result = DirectSoundCreate8(pointer<const GUID>(proxies, call.arg(0)), &host, nullptr);
        proxies.memory().write<std::uint32_t>(out, proxies.wrap(host));
        return code(result);
    };
    // TM imports it by ordinal
    functions.push_back(Function{ "dsound.dll", "#11", Convention::Stdcall, 3, directSound });
    functions.push_back(Function{ "dsound.dll", "DirectSoundCreate8", Convention::Stdcall, 3, directSound });
    addD3dx(proxies, functions);
    return functions;
}

} // namespace tmweb
