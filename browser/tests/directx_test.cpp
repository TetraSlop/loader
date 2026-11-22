// The DirectX proxies on compat's objects, called the way TM calls them. Wasm only, under Node.
#include "com/directx.h"
#include "com/proxies.h"

#include <d3d8.h>
#include <dinput.h>
#include <windows.h>

#include <mmsystem.h>

#include <dsound.h>

#include "dx/d3d8_null.h"
#include "tests/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

using namespace tmweb;

extern "C" auto kr_dinput_key(int key, bool down) -> void;
extern "C" auto kr_dinput_mouse(int dx, int dy, int dz) -> void;

namespace
{

// Vtable slots, as tmloader's src/graphics/d3d8_abi.h numbers them
enum Slot : unsigned
{
    kQueryInterface = 0,
    kRelease        = 2,

    kCreateDevice = 15,

    kReset              = 14,
    kPresent            = 15,
    kCreateTexture      = 20,
    kCreateVertexBuffer = 23,
    kBeginScene         = 34,
    kEndScene           = 35,
    kClear              = 36,
    kGetTexture         = 60,
    kSetTexture         = 61,
    kDrawPrimitive      = 70,
    kSetVertexShader    = 76,
    kSetStreamSource    = 83,
    kGetStreamSource    = 84,

    kBufferLock      = 11,
    kBufferUnlock    = 12,
    kGetSurfaceLevel = 15,
    kTextureLockRect = 16,
    kSurfaceLockRect = 9,
    kSurfaceUnlock   = 10,
    kUpdateTexture   = 29,
    kSoundPosition   = 4,
    kSoundPlay       = 12,
    kTextureUnlock   = 17,

    kInputCreateDevice   = 3,
    kSetDataFormat       = 11,
    kSetCooperativeLevel = 13,
    kAcquire             = 7,
    kGetDeviceState      = 9,

    kCreateSoundBuffer = 3,
    kSoundLock         = 11,
    kSoundUnlock       = 19,
};

constexpr std::uint32_t kWindow = 0x00010092;
constexpr DWORD         kFvf    = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1; // 0x1C4
constexpr UINT          kStride = 32;

// One guest arena per process, so every test shares this
auto publish(Machine& machine) -> bool
{
    machine.boundary().publish();
    return true;
}

// The machine's clock and what the stand-ins hand the host
std::uint64_t      g_now = 0;
std::vector<float> g_sound;
unsigned           g_presented = 0;
// The host's pointer for the mouse test, and where TM's cursor block would be
std::optional<Pointer> g_pointer;
std::uint32_t          g_cursor = 0;

auto media() -> MediaHost
{
    auto host      = MediaHost{};
    host.presented = []
    {
        ++g_presented;
    };
    host.sound = [](const std::span<const float> frames)
    {
        g_sound.insert(g_sound.end(), frames.begin(), frames.end());
    };
    host.soundRate = 48000;
    host.pointer   = []
    {
        return g_pointer;
    };
    host.gameCursor = g_cursor;
    return host;
}

auto clock(Machine& machine) -> bool
{
    machine.scheduler().setClock([]
                                 {
                                     return g_now;
                                 });
    return true;
}

struct World
{
    Machine               machine;
    bool                  published = publish(machine); // COM slots come after the module images
    bool                  clocked   = clock(machine);
    std::uint32_t         cursor = g_cursor = machine.memory().allocate(0x30);
    Proxies               proxies{ machine };
    std::vector<Function> exports = addDirectX(proxies, media());
    Context               context{};
    std::uint32_t         stack = machine.memory().allocate(0x10000);

    auto memory() -> Memory&
    {
        return machine.memory();
    }

    auto scratch(const std::uint32_t size) -> std::uint32_t
    {
        return memory().allocate(size);
    }

    template <class T>
    auto at(const std::uint32_t address) -> T*
    {
        return reinterpret_cast<T*>(memory().host(address));
    }

    // Push the arguments under a return address and run the handler, as the boundary does at a trap
    auto run(const Handler& handler, const std::string_view name, std::initializer_list<std::uint32_t> args) -> std::uint32_t
    {
        context.r[kEsp] = stack + 0x8000;
        auto address    = context.r[kEsp];
        memory().write<std::uint32_t>(address, 0xDEADBEEF);
        for (const auto arg : args)
        {
            memory().write<std::uint32_t>(address += 4, arg);
        }
        auto       call    = Call{ machine, context, name };
        const auto outcome = handler(call);
        if (const auto* halt = std::get_if<Halt>(&outcome))
        {
            FAIL(halt->reason);
        }
        REQUIRE(std::holds_alternative<Return>(outcome));
        return std::get<Return>(outcome).eax;
    }

    auto exported(const std::string_view name, std::initializer_list<std::uint32_t> args) -> std::uint32_t
    {
        const auto found = std::ranges::find(exports, name, &Function::name);
        REQUIRE(found != exports.end());
        CHECK(found->argc == args.size());
        return run(found->handler, name, args);
    }

    auto method(const std::uint32_t object, const unsigned slot, std::initializer_list<std::uint32_t> args = {})
        -> std::uint32_t
    {
        const auto* const method = proxies.slot(object, slot);
        REQUIRE(method != nullptr);
        CHECK(method->argc == 1 + args.size());
        auto all = std::vector<std::uint32_t>{ object };
        all.insert(all.end(), args);
        context.r[kEsp] = stack + 0x8000;
        auto address    = context.r[kEsp];
        memory().write<std::uint32_t>(address, 0xDEADBEEF);
        for (const auto arg : all)
        {
            memory().write<std::uint32_t>(address += 4, arg);
        }
        auto       call    = Call{ machine, context, method->name };
        const auto outcome = method->handler(call);
        if (const auto* halt = std::get_if<Halt>(&outcome))
        {
            FAIL(halt->reason);
        }
        REQUIRE(std::holds_alternative<Return>(outcome));
        return std::get<Return>(outcome).eax;
    }

    auto guid(const GUID& value) -> std::uint32_t
    {
        const auto address = scratch(sizeof(GUID));
        std::memcpy(at<GUID>(address), &value, sizeof(GUID));
        return address;
    }

    // What the out-pointer at address holds after a call
    auto out(const std::uint32_t address) -> std::uint32_t
    {
        return memory().read<std::uint32_t>(address);
    }

    auto presentParameters() -> std::uint32_t
    {
        const auto address            = scratch(sizeof(D3DPRESENT_PARAMETERS));
        auto&      params             = *at<D3DPRESENT_PARAMETERS>(address);
        params.BackBufferWidth        = 640;
        params.BackBufferHeight       = 480;
        params.BackBufferFormat       = D3DFMT_X8R8G8B8;
        params.BackBufferCount        = 1;
        params.SwapEffect             = D3DSWAPEFFECT_COPY;
        params.hDeviceWindow          = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(kWindow));
        params.Windowed               = TRUE;
        params.EnableAutoDepthStencil = FALSE;
        return address;
    }

    // Direct3DCreate8 and CreateDevice as TM makes them: windowed, behaviour 0x20
    auto device(const DWORD behaviour = D3DCREATE_SOFTWARE_VERTEXPROCESSING) -> std::uint32_t
    {
        const auto d3d = exported("Direct3DCreate8", { D3D_SDK_VERSION });
        REQUIRE(d3d != 0);
        const auto out = scratch(4);
        REQUIRE(method(d3d, kCreateDevice, { 0, D3DDEVTYPE_HAL, kWindow, behaviour, presentParameters(), out }) == D3D_OK);
        return this->out(out);
    }
};

auto world() -> World&
{
    static auto instance = World{};
    return instance;
}

} // namespace

static_assert(sizeof(D3DPRESENT_PARAMETERS) == 52, "D3D8 structs keep their x86 layout in Wasm");
static_assert(sizeof(D3DLOCKED_RECT) == 8);
static_assert(sizeof(DSBUFFERDESC) == 36);

TEST_CASE("CreateDevice sets the thread's x87 to 24-bit precision unless asked to preserve it")
{
    auto& w               = world();
    w.context.fpu.control = 0x027F;
    const auto device     = w.device();
    CHECK(device != 0);
    CHECK(w.context.fpu.control == 0x007F);

    w.context.fpu.control = 0x027F;
    w.device(D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE);
    CHECK(w.context.fpu.control == 0x027F);
}

TEST_CASE("Direct3DCreate8 wants the SDK version")
{
    CHECK(world().exported("Direct3DCreate8", { 120 }) == 0);
}

TEST_CASE("Pretransformed quads go through a locked dynamic vertex buffer")
{
    auto&      w      = world();
    const auto device = w.device();
    const auto out    = w.scratch(4);
    REQUIRE(w.method(device, kCreateVertexBuffer, { 4 * kStride, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT, out }) == D3D_OK);
    const auto buffer = w.out(out);
    REQUIRE(buffer != 0);

    REQUIRE(w.method(buffer, kBufferLock, { 0, 0, out, D3DLOCK_DISCARD }) == D3D_OK);
    const auto vertices = w.out(out);
    CHECK(w.memory().mapped(vertices, 4 * kStride));
    for (std::uint32_t i = 0; i < 4 * kStride; ++i)
    {
        w.memory().write<std::uint8_t>(vertices + i, static_cast<std::uint8_t>(i));
    }
    REQUIRE(w.method(buffer, kBufferUnlock) == D3D_OK);
    const auto& data = static_cast<nulldx::NullVertexBuffer*>(w.proxies.unwrap<IDirect3DVertexBuffer8>(buffer))->Data();
    REQUIRE(data.size() == 4 * kStride);
    CHECK(data[0] == 0);
    CHECK(data[127] == 127);

    // A range lock stages that range only
    REQUIRE(w.method(buffer, kBufferLock, { kStride, kStride, out, 0 }) == D3D_OK);
    w.memory().write<std::uint8_t>(w.out(out), 0xAA);
    REQUIRE(w.method(buffer, kBufferUnlock) == D3D_OK);
    CHECK(data[kStride] == 0xAA);
    CHECK(data[kStride - 1] == kStride - 1);

    CHECK(w.method(device, kSetStreamSource, { 0, buffer, kStride }) == D3D_OK);
    const auto stride = w.scratch(4);
    CHECK(w.method(device, kGetStreamSource, { 0, out, stride }) == D3D_OK);
    CHECK(w.out(out) == buffer);
    CHECK(w.out(stride) == kStride);
    w.method(buffer, kRelease);

    CHECK(w.method(device, kSetVertexShader, { kFvf }) == D3D_OK);
    CHECK(w.method(device, kClear, { 0, 0, D3DCLEAR_TARGET, 0xFF000000, std::bit_cast<std::uint32_t>(1.0f), 0 }) == D3D_OK);
    CHECK(w.method(device, kBeginScene) == D3D_OK);
    CHECK(w.method(device, kDrawPrimitive, { D3DPT_TRIANGLESTRIP, 0, 2 }) == D3D_OK);
    CHECK(w.method(device, kEndScene) == D3D_OK);
    CHECK(w.method(device, kPresent, { 0, 0, 0, 0 }) == D3D_OK);
    CHECK(w.method(device, kReset, { w.presentParameters() }) == D3D_OK);
}

TEST_CASE("A texture's lock stages its pixels and the unlock hands them to the device")
{
    auto&      w      = world();
    const auto device = w.device();
    const auto out    = w.scratch(4);
    REQUIRE(w.method(device, kCreateTexture, { 256, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out }) == D3D_OK);
    const auto texture = w.out(out);
    REQUIRE(texture != 0);

    const auto locked = w.scratch(sizeof(D3DLOCKED_RECT));
    REQUIRE(w.method(texture, kTextureLockRect, { 0, locked, 0, 0 }) == D3D_OK);
    const auto pitch = w.memory().read<std::int32_t>(locked);
    const auto bits  = w.memory().read<std::uint32_t>(locked + 4);
    CHECK(pitch == 256 * 4);
    CHECK(w.memory().mapped(bits, 128 * pitch));
    w.memory().write<std::uint32_t>(bits + 127 * pitch + 255 * 4, 0x80FF4020);
    REQUIRE(w.method(texture, kTextureUnlock, { 0 }) == D3D_OK);

    auto* const host = w.proxies.unwrap<IDirect3DTexture8>(texture);
    auto        mine = D3DLOCKED_RECT{};
    REQUIRE(host->LockRect(0, &mine, nullptr, D3DLOCK_READONLY) == D3D_OK);
    CHECK(*reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(mine.pBits) + 127 * pitch + 255 * 4) == 0x80FF4020);
    host->UnlockRect(0);

    // A rectangle: pBits points at its corner, and the last row ends at its right edge
    const auto rect   = w.scratch(sizeof(RECT));
    *w.at<RECT>(rect) = RECT{ 16, 8, 32, 24 };
    REQUIRE(w.method(texture, kTextureLockRect, { 0, locked, rect, 0 }) == D3D_OK);
    const auto corner = w.memory().read<std::uint32_t>(locked + 4);
    w.memory().write<std::uint32_t>(corner + 15 * pitch + 15 * 4, 0x12345678);
    REQUIRE(w.method(texture, kTextureUnlock, { 0 }) == D3D_OK);
    REQUIRE(host->LockRect(0, &mine, nullptr, D3DLOCK_READONLY) == D3D_OK);
    CHECK(*reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(mine.pBits) + 23 * pitch + 31 * 4) == 0x12345678);
    host->UnlockRect(0);

    // SetTexture unwraps it; GetTexture hands back the same proxy through IDirect3DBaseTexture8
    CHECK(w.method(device, kSetTexture, { 0, texture }) == D3D_OK);
    CHECK(w.method(device, kGetTexture, { 0, out }) == D3D_OK);
    CHECK(w.out(out) == texture);
    w.method(texture, kRelease);
    CHECK(w.method(device, kSetTexture, { 0, 0 }) == D3D_OK);
}

TEST_CASE("The keyboard reports what the host presses")
{
    auto&      w   = world();
    const auto out = w.scratch(4);
    REQUIRE(w.exported("DirectInput8Create", { 0x10000000, DIRECTINPUT_VERSION, w.guid(IID_IDirectInput8A), out, 0 }) == DI_OK);
    const auto input = w.out(out);
    REQUIRE(input != 0);
    REQUIRE(w.method(input, kInputCreateDevice, { w.guid(GUID_SysKeyboard), out, 0 }) == DI_OK);
    const auto keyboard = w.out(out);
    REQUIRE(keyboard != 0);

    const auto format           = w.scratch(sizeof(DIDATAFORMAT));
    *w.at<DIDATAFORMAT>(format) = DIDATAFORMAT{ sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), 0, 256, 0, nullptr };
    CHECK(w.method(keyboard, kSetDataFormat, { format }) == DI_OK);
    CHECK(w.method(keyboard, kSetCooperativeLevel, { kWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE }) == DI_OK);
    CHECK(w.method(keyboard, kAcquire) == DI_OK);

    kr_dinput_key(DIK_A, true);
    const auto keys = w.scratch(256);
    CHECK(w.method(keyboard, kGetDeviceState, { 256, keys }) == DI_OK);
    CHECK(w.memory().read<std::uint8_t>(keys + DIK_A) == 0x80);
    CHECK(w.memory().read<std::uint8_t>(keys + DIK_B) == 0);
    kr_dinput_key(DIK_A, false);
}

TEST_CASE("A DirectSound buffer from ordinal 11 locks through staging")
{
    auto&      w   = world();
    const auto out = w.scratch(4);
    REQUIRE(w.exported("#11", { 0, out, 0 }) == DS_OK);
    const auto sound = w.out(out);
    REQUIRE(sound != 0);

    const auto format           = w.scratch(sizeof(WAVEFORMATEX));
    *w.at<WAVEFORMATEX>(format) = WAVEFORMATEX{ WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0 };
    const auto desc             = w.scratch(sizeof(DSBUFFERDESC));
    auto&      bufferDesc       = *w.at<DSBUFFERDESC>(desc);
    bufferDesc                  = DSBUFFERDESC{};
    bufferDesc.dwSize           = sizeof(DSBUFFERDESC);
    bufferDesc.dwFlags          = DSBCAPS_CTRLVOLUME;
    bufferDesc.dwBufferBytes    = 4096;
    bufferDesc.lpwfxFormat      = w.at<WAVEFORMATEX>(format);
    REQUIRE(w.method(sound, kCreateSoundBuffer, { desc, out, 0 }) == DS_OK);
    const auto buffer = w.out(out);
    REQUIRE(buffer != 0);

    const auto first = w.scratch(4);
    const auto bytes = w.scratch(4);
    REQUIRE(w.method(buffer, kSoundLock, { 1024, 512, first, bytes, 0, 0, 0 }) == DS_OK);
    REQUIRE(w.out(bytes) == 512);
    w.memory().fill(w.out(first), 0x5A, 512);
    REQUIRE(w.method(buffer, kSoundUnlock, { w.out(first), 512, 0, 0 }) == DS_OK);

    auto* const host = w.proxies.unwrap<IDirectSoundBuffer>(buffer);
    auto*       mine = static_cast<void*>(nullptr);
    auto        size = DWORD{ 0 };
    REQUIRE(host->Lock(1024, 512, &mine, &size, nullptr, nullptr, 0) == DS_OK);
    CHECK(static_cast<const BYTE*>(mine)[0] == 0x5A);
    CHECK(static_cast<const BYTE*>(mine)[511] == 0x5A);
    host->Unlock(mine, size, nullptr, 0);

    // QueryInterface for IDirectSoundBuffer8 gives a second proxy of the same buffer
    REQUIRE(w.method(buffer, kQueryInterface, { w.guid(IID_IDirectSoundBuffer8), out }) == DS_OK);
    const auto buffer8 = w.out(out);
    CHECK(buffer8 != 0);
    CHECK(buffer8 != buffer);
    CHECK(static_cast<void*>(w.proxies.unwrap<IDirectSoundBuffer8>(buffer8)) == static_cast<void*>(host));
}

TEST_CASE("Guest x86 code calls the proxies through their vtables")
{
    auto&      w      = world();
    auto&      memory = w.memory();
    const auto d3d    = w.exported("Direct3DCreate8", { D3D_SDK_VERSION });
    REQUIRE(d3d != 0);

    // A stand-in to stop at, called through a one-slot vtable
    const auto stop = std::array{ Method{ "done", 1, [](Call&) -> Outcome
                                          {
                                              return Halt{ "done" };
                                          } } };
    const auto done = w.machine.boundary().addInterface("test", stop);

    constexpr auto code   = std::uint32_t{ 0x10001000 };
    constexpr auto data   = std::uint32_t{ 0x10002000 };
    const auto     params = w.presentParameters();
    memory.map(code, 0x2000);
    memory.write<std::uint32_t>(data, d3d);
    memory.write<std::uint32_t>(data + 4, data + 16); // an object whose vtable is done
    memory.write<std::uint32_t>(data + 16, done);

    auto       program = std::vector<std::uint8_t>{};
    const auto emit    = [&](std::initializer_list<std::uint8_t> bytes)
    {
        program.insert(program.end(), bytes);
    };
    const auto emit32 = [&](const std::uint32_t value)
    {
        emit({ static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24) });
    };
    emit({ 0x8B, 0x35 }); // mov esi, [data]: the IDirect3D8
    emit32(data);
    emit({ 0x89, 0xE5 });        // mov ebp, esp
    emit({ 0x56, 0x8B, 0x06 });  // push esi; mov eax, [esi]
    emit({ 0xFF, 0x50, 4 * 4 }); // call [eax + GetAdapterCount]
    emit({ 0x89, 0xC3 });        // mov ebx, eax
    emit({ 0x68 });              // push out
    emit32(data + 8);
    emit({ 0x68 }); // push params
    emit32(params);
    emit({ 0x6A, D3DCREATE_SOFTWARE_VERTEXPROCESSING }); // push 0x20
    emit({ 0x68 });                                      // push window
    emit32(kWindow);
    emit({ 0x6A, D3DDEVTYPE_HAL, 0x6A, 0 }); // push HAL; push adapter 0
    emit({ 0x56, 0x8B, 0x06 });              // push esi; mov eax, [esi]
    emit({ 0xFF, 0x50, 4 * kCreateDevice }); // call [eax + CreateDevice]
    emit({ 0x89, 0xC7 });                    // mov edi, eax
    emit({ 0xD9, 0x3D });                    // fnstcw [data + 12]
    emit32(data + 12);
    emit({ 0x89, 0xE9 }); // mov ecx, ebp: the calls popped their arguments
    emit({ 0xA1 });       // mov eax, [data + 4]
    emit32(data + 4);
    emit({ 0x50, 0x8B, 0x00, 0xFF, 0x10 }); // push eax; mov eax, [eax]; call [eax]
    memory.write(code, std::span<const std::uint8_t>{ program });

    auto& thread               = w.machine.scheduler().createMain(0x10000);
    thread.context.eip         = code;
    thread.context.fpu.control = 0x027F;
    const auto result          = test::Engine{ w.machine }.run(100000);
    CHECK(result.reason == "done");
    CHECK(thread.context.r[kEbx] == 1);
    CHECK(thread.context.r[kEdi] == D3D_OK);
    CHECK(thread.context.r[kEcx] == thread.context.r[kEbp]);
    CHECK(memory.read<std::uint16_t>(data + 12) == 0x007F);
    const auto device = memory.read<std::uint32_t>(data + 8);
    CHECK(w.proxies.unwrap<IDirect3DDevice8>(device) != nullptr);
}

TEST_CASE("UpdateTexture copies what the game drew into system memory to the texture it draws with")
{
    auto&      w      = world();
    const auto device = w.device();
    const auto out    = w.scratch(4);
    REQUIRE(w.method(device, kCreateTexture, { 0xAA, 0x20, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, out }) == D3D_OK);
    const auto staging = w.out(out);
    REQUIRE(w.method(device, kCreateTexture, { 0xAA, 0x20, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, out }) == D3D_OK);
    const auto drawn = w.out(out);

    REQUIRE(w.method(staging, kGetSurfaceLevel, { 0, out }) == D3D_OK);
    const auto surface = w.out(out);
    const auto locked  = w.scratch(sizeof(D3DLOCKED_RECT));
    REQUIRE(w.method(surface, kSurfaceLockRect, { locked, 0, 0 }) == D3D_OK);
    const auto pitch = w.memory().read<std::int32_t>(locked);
    const auto bits  = w.memory().read<std::uint32_t>(locked + 4);
    w.memory().write<std::uint32_t>(bits + 31 * pitch + 0xA9 * 4, 0x80FFFFFF);
    REQUIRE(w.method(surface, kSurfaceUnlock) == D3D_OK);
    w.method(surface, kRelease);

    REQUIRE(w.method(device, kUpdateTexture, { staging, drawn }) == D3D_OK);
    auto* const host = w.proxies.unwrap<IDirect3DTexture8>(drawn);
    auto        mine = D3DLOCKED_RECT{};
    REQUIRE(host->LockRect(0, &mine, nullptr, D3DLOCK_READONLY) == D3D_OK);
    CHECK(*reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(mine.pBits) + 31 * pitch + 0xA9 * 4) == 0x80FFFFFF);
    host->UnlockRect(0);
    w.method(staging, kRelease);
    w.method(drawn, kRelease);
}

TEST_CASE("A playing buffer is mixed for the host as the clock runs, and Present reports the frame")
{
    auto&      w   = world();
    const auto out = w.scratch(4);
    REQUIRE(w.exported("#11", { 0, out, 0 }) == DS_OK);
    const auto sound = w.out(out);

    const auto format           = w.scratch(sizeof(WAVEFORMATEX));
    *w.at<WAVEFORMATEX>(format) = WAVEFORMATEX{ WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
    const auto desc             = w.scratch(sizeof(DSBUFFERDESC));
    auto&      bufferDesc       = *w.at<DSBUFFERDESC>(desc);
    bufferDesc                  = DSBUFFERDESC{};
    bufferDesc.dwSize           = sizeof(DSBUFFERDESC);
    bufferDesc.dwBufferBytes    = 0xC000;
    bufferDesc.lpwfxFormat      = w.at<WAVEFORMATEX>(format);
    REQUIRE(w.method(sound, kCreateSoundBuffer, { desc, out, 0 }) == DS_OK);
    const auto buffer = w.out(out);

    const auto first = w.scratch(4);
    const auto bytes = w.scratch(4);
    REQUIRE(w.method(buffer, kSoundLock, { 0, 0xC000, first, bytes, 0, 0, 0 }) == DS_OK);
    for (auto sample = 0u; sample < 0xC000 / 2; ++sample)
    {
        w.memory().write<std::int16_t>(w.out(first) + 2 * sample, 0x4000);
    }
    REQUIRE(w.method(buffer, kSoundUnlock, { w.out(first), 0xC000, 0, 0 }) == DS_OK);

    g_sound.clear();
    REQUIRE(w.method(buffer, kSoundPlay, { 0, 0, DSBPLAY_LOOPING }) == DS_OK);
    g_now += 100;
    const auto position = w.scratch(4);
    REQUIRE(w.method(buffer, kSoundPosition, { position, 0 }) == DS_OK);
    CHECK(g_sound.size() == 2 * 4800);
    CHECK(std::ranges::any_of(g_sound, [](const float sample)
                              {
                                  return sample > 0.1f;
                              }));
    CHECK(w.out(position) == 4800 * 4);

    const auto device    = w.device();
    const auto presented = g_presented;
    g_now += 10;
    REQUIRE(w.method(device, kPresent, { 0, 0, 0, 0 }) == D3D_OK);
    CHECK(g_presented == presented + 1);
    CHECK(g_sound.size() == 2 * (4800 + 480));
    w.method(buffer, kRelease);
}

TEST_CASE("The mouse steers TM's cursor onto the host's pointer")
{
    auto&      w      = world();
    const auto cursor = w.cursor;
    const auto out    = w.scratch(4);
    REQUIRE(w.exported("DirectInput8Create", { 0x10000000, DIRECTINPUT_VERSION, w.guid(IID_IDirectInput8A), out, 0 }) == DI_OK);
    const auto input = w.out(out);
    REQUIRE(w.method(input, kInputCreateDevice, { w.guid(GUID_SysMouse), out, 0 }) == DI_OK);
    const auto mouse = w.out(out);
    REQUIRE(w.method(mouse, kAcquire) == DI_OK);

    w.memory().write<std::int16_t>(cursor + 0x2C, 100);
    w.memory().write<std::int16_t>(cursor + 0x2E, 50);
    w.memory().write<std::int32_t>(cursor + 0x14, 3);
    g_pointer        = Pointer{ 300, 200 };
    const auto state = w.scratch(sizeof(DIMOUSESTATE));
    REQUIRE(w.method(mouse, kGetDeviceState, { sizeof(DIMOUSESTATE), state }) == DI_OK);
    CHECK(w.memory().read<std::int32_t>(state) == 200);
    CHECK(w.memory().read<std::int32_t>(state + 4) == 150);
    CHECK(w.memory().read<std::int32_t>(cursor + 0x14) == 0);

    // Unmoved, the deltas are DirectInput's own, and reading clears them
    kr_dinput_mouse(5, -2, 0);
    REQUIRE(w.method(mouse, kGetDeviceState, { sizeof(DIMOUSESTATE), state }) == DI_OK);
    CHECK(w.memory().read<std::int32_t>(state) == 5);
    CHECK(w.memory().read<std::int32_t>(state + 4) == -2);
    REQUIRE(w.method(mouse, kGetDeviceState, { sizeof(DIMOUSESTATE), state }) == DI_OK);
    CHECK(w.memory().read<std::int32_t>(state) == 0);
    CHECK(w.memory().read<std::int32_t>(state + 4) == 0);
    g_pointer.reset();
}
