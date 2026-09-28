#include "app/runtime.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <stdexcept>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static void checked(HRESULT result, const char* operation)
{
    printf("%s=%08lX\n", operation, result);
    INFO(operation);
    REQUIRE(SUCCEEDED(result));
}

static void checkBuffer(IDirectSound8* sound, DWORD flags, DWORD bytes)
{
    WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0 };
    DSBUFFERDESC desc{};
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

TEST_CASE("Native music and effect buffers support required interfaces", "[audio-device]")
{
    // Uses Windows' audio device, but creates no window and never calls Play.
    // Match the game's process-local integrity setting as well as its buffers.
    REQUIRE(tmloader::lowerIntegrity());
    ComPtr<IDirectSound8> sound;
    const auto            result = tmloader::tracedSound8(nullptr, sound.GetAddressOf(), nullptr);
    if (result == DSERR_NODRIVER)
    {
        SKIP("no audio output device");
    }
    checked(result, "Audio device");
    checked(sound->SetCooperativeLevel(GetDesktopWindow(), DSSCL_NORMAL), "SetCooperativeLevel");
    // Exact flags/sizes at TM +1AAF9C/+1AAFA9 (BGM), +1AA901/+1AA90E (SE).
    checkBuffer(sound.Get(), 0x50080, 0xc0000);
    checkBuffer(sound.Get(), 0x500c0, 0x30000);
    puts("PASS music and effect buffers; no playback requested");
}
