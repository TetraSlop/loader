#include "platform/audio.cpp"
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{

// Exact flags and sizes the game passes at TM +1AAF9C/+1AAFA9 (BGM) and +1AA901/+1AA90E (SE).
constexpr DWORD kMusicFlags  = 0x50080;
constexpr DWORD kMusicBytes  = 0xc0000;
constexpr DWORD kEffectFlags = 0x500c0;
constexpr DWORD kEffectBytes = 0x30000;

} // namespace

namespace tmloader
{

void setImport(HMODULE, DWORD, void*)
{
}

} // namespace tmloader

namespace
{

void checked(HRESULT result, const char* operation)
{
    INFO(operation);
    REQUIRE(SUCCEEDED(result));
}

void checkBuffer(IDirectSound8* sound, DWORD flags, DWORD bytes)
{
    auto format        = WAVEFORMATEX{ WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0 };
    auto desc          = DSBUFFERDESC{};
    desc.dwSize        = sizeof(desc);
    desc.dwFlags       = flags;
    desc.dwBufferBytes = bytes;
    desc.lpwfxFormat   = &format;
    ComPtr<IDirectSoundBuffer> buffer;
    checked(sound->CreateSoundBuffer(&desc, buffer.GetAddressOf(), nullptr), "CreateSoundBuffer");
    // TM requires this interface after creating both music and effect buffers.
    ComPtr<IDirectSoundBuffer8> buffer8;
    checked(buffer->QueryInterface(IID_IDirectSoundBuffer8,
                                   reinterpret_cast<void**>(buffer8.GetAddressOf())),
            "QueryInterface(IDirectSoundBuffer8)");
    void* samples = nullptr;
    DWORD size    = 0;
    checked(buffer8->Lock(0, bytes, &samples, &size, nullptr, nullptr, 0), "Lock");
    ZeroMemory(samples, size);
    checked(buffer8->Unlock(samples, size, nullptr, 0), "Unlock");
    DWORD status = 0;
    checked(buffer8->GetStatus(&status), "GetStatus");
    REQUIRE((status & DSBSTATUS_PLAYING) == 0);
}

} // namespace

TEST_CASE("Native music and effect buffers support required interfaces", "[audio-device]")
{
    // Uses Windows' audio device, but creates no window and never calls Play.
    // Match the game's process-local integrity setting as well as its buffers.
    REQUIRE(tmloader::lowerIntegrity());
    ComPtr<IDirectSound8> sound;
    const auto            result = tmloader::hookedDirectSoundCreate8(nullptr, sound.GetAddressOf(), nullptr);
    if (result == DSERR_NODRIVER)
    {
        SKIP("no audio output device");
    }

    checked(result, "Audio device");
    checked(sound->SetCooperativeLevel(GetDesktopWindow(), DSSCL_NORMAL), "SetCooperativeLevel");
    checkBuffer(sound.Get(), kMusicFlags, kMusicBytes);
    checkBuffer(sound.Get(), kEffectFlags, kEffectBytes);
}
