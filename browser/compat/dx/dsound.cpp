// kr_dsound_mix moves each play cursor on as it mixes, so the client's streaming threads refill buffers as with a card.

// dsound.h takes WAVEFORMATEX from mmsystem.h, as the SDK's does.
#include <windows.h>

#include <mmsystem.h>

#include <dsound.h>

#include "com.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace
{

class SoundBuffer;
auto buffers = std::vector<SoundBuffer*>{};

// IDirectSoundBuffer8's methods begin with IDirectSoundBuffer's, so one object is both, as in DirectSound.
auto AsBuffer(IDirectSoundBuffer8* buffer) -> LPDIRECTSOUNDBUFFER
{
    return reinterpret_cast<LPDIRECTSOUNDBUFFER>(buffer);
}

// Volume and pan are hundredths of a decibel of attenuation; DSBVOLUME_MIN is silence
auto Gain(LONG hundredths) -> float
{
    if (hundredths <= DSBVOLUME_MIN)
    {
        return 0.0f;
    }

    return std::pow(10.0f, hundredths / 2000.0f);
}

class SoundBuffer final : public nulldx::Object<IDirectSoundBuffer8>
{
public:
    SoundBuffer(const DSBUFFERDESC& desc, std::shared_ptr<std::vector<BYTE>> data)
    : flags_(desc.dwFlags)
    , data_(std::move(data))
    {
        if (desc.lpwfxFormat != nullptr)
        {
            format_ = *desc.lpwfxFormat;
        }
        else
        {
            format_ = { WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0 };
        }

        buffers.push_back(this);
    }

    SoundBuffer(const SoundBuffer& other)
    : flags_(other.flags_)
    , format_(other.format_)
    , data_(other.data_)
    , volume_(other.volume_)
    , pan_(other.pan_)
    , frequency_(other.frequency_)
    {
        buffers.push_back(this);
    }

    SoundBuffer(SoundBuffer&&) = delete;

    auto operator=(const SoundBuffer&) -> SoundBuffer& = delete;

    auto operator=(SoundBuffer&&) -> SoundBuffer& = delete;

    ~SoundBuffer() override
    {
        buffers.erase(std::find(buffers.begin(), buffers.end(), this));
    }

    auto Implements(REFIID riid) const -> bool override
    {
        return riid == IID_IDirectSoundBuffer || riid == IID_IDirectSoundBuffer8;
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS caps) override
    {
        return nulldx::Store(caps, { sizeof(DSBCAPS), flags_, Size(), 0, 0 }, DSERR_INVALIDPARAM);
    }

    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD play, LPDWORD write) override
    {
        const auto position = Position();
        if (play != nullptr)
        {
            *play = position;
        }

        if (write != nullptr)
        {
            *write = position;
        }

        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX format, DWORD size, LPDWORD written) override
    {
        constexpr auto needed = DWORD{ sizeof(WAVEFORMATEX) };
        if (format == nullptr)
        {
            return nulldx::Store(written, needed, DSERR_INVALIDPARAM);
        }

        std::memcpy(format, &format_, std::min(size, needed));
        if (written != nullptr)
        {
            *written = std::min(size, needed);
        }

        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetVolume(LPLONG volume) override
    {
        return nulldx::Store(volume, volume_, DSERR_INVALIDPARAM);
    }

    HRESULT STDMETHODCALLTYPE GetPan(LPLONG pan) override
    {
        return nulldx::Store(pan, pan_, DSERR_INVALIDPARAM);
    }

    HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD frequency) override
    {
        return nulldx::Store(frequency, Frequency(), DSERR_INVALIDPARAM);
    }

    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD status) override
    {
        const auto current = [&]() -> DWORD
        {
            if (!playing_)
            {
                return 0;
            }

            if (looping_)
            {
                return DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;
            }

            return DSBSTATUS_PLAYING;
        }();

        return nulldx::Store(status, current, DSERR_INVALIDPARAM);
    }

    HRESULT STDMETHODCALLTYPE Initialize(IDirectSound* sound, LPCDSBUFFERDESC desc) override
    {
        return DSERR_ALREADYINITIALIZED;
    }

    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID* first, LPDWORD firstBytes, LPVOID* second, LPDWORD secondBytes, DWORD flags) override
    {
        const auto size = Size();
        if ((flags & DSBLOCK_FROMWRITECURSOR) != 0)
        {
            offset = Position();
        }

        if ((flags & DSBLOCK_ENTIREBUFFER) != 0)
        {
            bytes = size;
        }

        if (first == nullptr || firstBytes == nullptr || offset >= size || bytes == 0 || bytes > size)
        {
            return DSERR_INVALIDPARAM;
        }

        *first             = data_->data() + offset;
        *firstBytes        = std::min(bytes, size - offset);
        const auto wrapped = bytes - *firstBytes;
        if (second != nullptr)
        {
            *second = [&]() -> LPVOID
            {
                if (wrapped == 0)
                {
                    return nullptr;
                }

                return data_->data();
            }();
        }

        if (secondBytes != nullptr)
        {
            *secondBytes = wrapped;
        }

        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Play(DWORD reserved, DWORD priority, DWORD flags) override
    {
        playing_ = true;
        looping_ = (flags & DSBPLAY_LOOPING) != 0;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD position) override
    {
        if (position >= std::max<DWORD>(Size(), 1))
        {
            return DSERR_INVALIDPARAM;
        }

        cursor_ = position / Align();
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFormat(const WAVEFORMATEX* format) override
    {
        if (format == nullptr)
        {
            return DSERR_INVALIDPARAM;
        }

        if ((flags_ & DSBCAPS_PRIMARYBUFFER) == 0)
        {
            return DSERR_INVALIDCALL;
        }

        format_ = *format;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetVolume(LONG volume) override
    {
        if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX)
        {
            return DSERR_INVALIDPARAM;
        }

        volume_ = volume;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetPan(LONG pan) override
    {
        if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT)
        {
            return DSERR_INVALIDPARAM;
        }

        pan_ = pan;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD frequency) override
    {
        if (frequency != DSBFREQUENCY_ORIGINAL && (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX))
        {
            return DSERR_INVALIDPARAM;
        }

        frequency_ = frequency;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override
    {
        playing_ = false;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Unlock(LPVOID first, DWORD firstBytes, LPVOID second, DWORD secondBytes) override
    {
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Restore() override
    {
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFX(DWORD count, LPDSEFFECTDESC effects, LPDWORD results) override
    {
        if (count != 0)
        {
            return DSERR_FXUNAVAILABLE;
        }

        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE AcquireResources(DWORD flags, DWORD count, LPDWORD results) override
    {
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID object, DWORD index, REFGUID iid, LPVOID* out) override
    {
        if (out != nullptr)
        {
            *out = nullptr;
        }

        return DSERR_OBJECTNOTFOUND;
    }

    // The client streams music into buffers without pan, and every other sound into buffers with it
    auto IsMusic() const -> bool
    {
        return (flags_ & DSBCAPS_CTRLPAN) == 0;
    }

    // With out null, or a format the mixer does not read, the buffer plays on unheard.
    auto Mix(float* out, uint32_t frames, uint32_t rate, float gain) -> void
    {
        const auto count = static_cast<size_t>(Size() / Align());
        if (!playing_ || count == 0)
        {
            return;
        }

        const auto step     = static_cast<double>(Frequency()) / rate;
        const auto channels = format_.nChannels;
        const auto bits     = format_.wBitsPerSample;
        if (out == nullptr || gain <= 0 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16))
        {
            Advance(step * frames, count);
            return;
        }

        const auto volume = gain * Gain(volume_);
        // Pan attenuates the far side only
        const auto left  = volume * Gain(std::min<LONG>(0, -pan_));
        const auto right = volume * Gain(std::min<LONG>(0, pan_));
        for (uint32_t i = 0; i < frames && playing_; ++i)
        {
            const auto at   = static_cast<size_t>(cursor_);
            const auto next = [&]() -> size_t
            {
                if (at + 1 < count)
                {
                    return at + 1;
                }

                if (looping_)
                {
                    return 0;
                }

                return at;
            }();

            const auto t  = static_cast<float>(cursor_ - at);
            const auto l0 = Sample(at, 0);
            const auto l1 = Sample(next, 0);
            const auto r0 = Sample(at, channels - 1);
            const auto r1 = Sample(next, channels - 1);
            out[2 * i] += (l0 + (l1 - l0) * t) * left;
            out[2 * i + 1] += (r0 + (r1 - r0) * t) * right;
            Advance(step, count);
        }
    }

private:
    auto Size() const -> DWORD
    {
        return static_cast<DWORD>(data_->size());
    }

    auto Align() const -> DWORD
    {
        return std::max<WORD>(format_.nBlockAlign, 1);
    }

    auto Frequency() const -> DWORD
    {
        if (frequency_ == DSBFREQUENCY_ORIGINAL)
        {
            return format_.nSamplesPerSec;
        }

        return frequency_;
    }

    auto Position() const -> DWORD
    {
        return static_cast<DWORD>(cursor_) * Align();
    }

    // A buffer played once stops at its end and goes back to its start
    auto Advance(double frames, size_t count) -> void
    {
        cursor_ += frames;
        if (cursor_ < count)
        {
            return;
        }

        if (looping_)
        {
            cursor_ = std::fmod(cursor_, static_cast<double>(count));
            return;
        }

        playing_ = false;
        cursor_  = 0;
    }

    auto Sample(size_t frame, WORD channel) const -> float
    {
        const auto* p = data_->data() + frame * Align() + channel * (format_.wBitsPerSample / 8);
        if (format_.wBitsPerSample == 8)
        {
            return (p[0] - 128) / 128.0f;
        }

        return static_cast<int16_t>(p[0] | p[1] << 8) / 32768.0f;
    }

    DWORD        flags_;
    WAVEFORMATEX format_;
    // Shared with the buffer's duplicates, which either side may outlive.
    std::shared_ptr<std::vector<BYTE>> data_;
    LONG                               volume_    = DSBVOLUME_MAX;
    LONG                               pan_       = DSBPAN_CENTER;
    DWORD                              frequency_ = DSBFREQUENCY_ORIGINAL;
    bool                               playing_   = false;
    bool                               looping_   = false;
    // The play cursor in frames, with the fraction resampling leaves
    double cursor_ = 0;
};

class DirectSound final : public nulldx::Object<IDirectSound8>
{
public:
    bool Implements(REFIID riid) const override
    {
        return riid == IID_IDirectSound || riid == IID_IDirectSound8;
    }

    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(
        LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* buffer, LPUNKNOWN outer) override
    {
        if (outer != nullptr)
        {
            return DSERR_NOAGGREGATION;
        }

        if (desc == nullptr || buffer == nullptr)
        {
            return DSERR_INVALIDPARAM;
        }

        *buffer            = nullptr;
        const auto primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
        if (!primary && (desc->lpwfxFormat == nullptr || desc->dwBufferBytes < DSBSIZE_MIN || desc->dwBufferBytes > DSBSIZE_MAX))
        {
            return DSERR_INVALIDPARAM;
        }

        const auto bytes = [&]() -> DWORD
        {
            if (primary)
            {
                return 0;
            }

            return desc->dwBufferBytes;
        }();

        *buffer = AsBuffer(new SoundBuffer(*desc, std::make_shared<std::vector<BYTE>>(bytes)));
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS caps) override
    {
        if (caps == nullptr)
        {
            return DSERR_INVALIDPARAM;
        }

        *caps                          = {};
        caps->dwSize                   = sizeof(DSCAPS);
        caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
        caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
        caps->dwPrimaryBuffers         = 1;
        return DS_OK;
    }

    // The copy shares the samples but plays on its own, as in DirectSound.
    HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(
        LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* duplicate) override
    {
        if (original == nullptr || duplicate == nullptr)
        {
            return DSERR_INVALIDPARAM;
        }

        *duplicate = AsBuffer(new SoundBuffer(*static_cast<SoundBuffer*>(reinterpret_cast<IDirectSoundBuffer8*>(original))));
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD level) override
    {
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Compact() override
    {
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD config) override
    {
        return nulldx::Store(config, DSSPEAKER_STEREO, DSERR_INVALIDPARAM);
    }

    HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD config) override
    {
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID device) override
    {
        return DSERR_ALREADYINITIALIZED;
    }

    HRESULT STDMETHODCALLTYPE VerifyCertification(LPDWORD certified) override
    {
        return nulldx::Store(certified, DS_CERTIFIED, DSERR_INVALIDPARAM);
    }
};

} // namespace

HRESULT WINAPI DirectSoundCreate8(LPCGUID pcGuidDevice, LPDIRECTSOUND8* ppDS8, LPUNKNOWN pUnkOuter)
{
    if (ppDS8 == nullptr)
    {
        return DSERR_INVALIDPARAM;
    }

    if (pUnkOuter != nullptr)
    {
        return DSERR_NOAGGREGATION;
    }

    *ppDS8 = new DirectSound();
    return DS_OK;
}

// out is stereo interleaved; with out null the buffers play on unheard.
extern "C" auto kr_dsound_mix(float* out, uint32_t frames, uint32_t rate, float music, float effects) -> void
{
    if (out != nullptr)
    {
        std::fill(out, out + 2 * static_cast<size_t>(frames), 0.0f);
    }

    for (auto* const buffer : buffers)
    {
        const auto gain = [&]() -> float
        {
            if (buffer->IsMusic())
            {
                return music;
            }

            return effects;
        }();

        buffer->Mix(out, frames, rate, gain);
    }
}
