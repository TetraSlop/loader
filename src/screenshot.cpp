#include "screenshot.h"
#include "runtime.h"
#include "screenshot_capture.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

namespace tmloader
{
namespace
{

using Microsoft::WRL::ComPtr;
std::atomic<bool>     pending{ false };
std::atomic<unsigned> sequence{ 0 };
std::mutex            statusMutex;
std::string           status;

void finishStatus(const std::string& message)
{
    std::lock_guard<std::mutex> lock(statusMutex);
    // A request arriving during compression belongs to the next frame. Keep
    // its queued state visible instead of replacing it with this frame's result.
    if (!pending.load())
    {
        status = message;
    }
}

void check(HRESULT hr, const char* action)
{
    if (FAILED(hr))
    {
        char message[160];
        sprintf_s(message, "%s (0x%08lX)", action, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

template <class... Args>
HRESULT call(void* object, unsigned slot, Args... args)
{
    using Method = HRESULT(WINAPI*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(object, args...);
}

} // namespace

namespace detail
{

ComScope::ComScope()
: result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
{
    if (result_ != RPC_E_CHANGED_MODE)
    {
        check(result_, "Initialize PNG encoder");
    }
}

ComScope::~ComScope()
{
    if (SUCCEEDED(result_))
    {
        CoUninitialize();
    }
}

LockedSurface::LockedSurface(void* surface)
: surface_(surface)
{
    check(call(surface_, 9, &pixels_, static_cast<const RECT*>(nullptr), DWORD{ 0x10 }),
          "Read game backbuffer"); // D3DLOCK_READONLY
}

LockedSurface::~LockedSurface()
{
    if (surface_)
    {
        call(surface_, 10);
    }
}

auto LockedSurface::pixels() const noexcept -> const SurfacePixels&
{
    return pixels_;
}

void LockedSurface::unlock()
{
    auto* const surface = surface_;
    surface_            = nullptr;
    check(call(surface, 10), "Unlock game backbuffer");
}

} // namespace detail

namespace
{

using detail::ComScope;
using detail::LockedSurface;

// Copy pixels before encoding, so the GPU resource is unlocked throughout the
// potentially slower compression and disk write. PNG has no unreliable alpha.
auto encodeBackbuffer(void* device) -> ComPtr<IStream>
{
    if (!device)
    {
        throw std::runtime_error("Game renderer is unavailable");
    }
    ComPtr<IUnknown> surface;
    check(call(device, 16, UINT{ 0 }, DWORD{ 0 }, surface.GetAddressOf()), "Get game backbuffer");
    if (!surface)
    {
        throw std::runtime_error("Game backbuffer is unavailable");
    }
    DWORD desc[8]{};
    check(call(surface.Get(), 8, desc), "Read game backbuffer dimensions");
    const auto width  = UINT{ desc[6] };
    const auto height = UINT{ desc[7] };
    if (desc[0] != 21 && desc[0] != 22) // A8R8G8B8 / X8R8G8B8
    {
        throw std::runtime_error("Screenshot requires a 32-bit game backbuffer");
    }
    const auto bytes = uint64_t{ width } * height * 3;
    if (!width || !height || width > 16384 || height > 16384 || bytes > 256 * 1024 * 1024)
    {
        throw std::runtime_error("Invalid game backbuffer dimensions");
    }
    std::vector<BYTE> bgr(static_cast<size_t>(bytes));
    {
        LockedSurface locked(surface.Get());
        const auto    pitch    = int64_t{ locked.pixels().pitch };
        const auto    rowBytes = [pitch]() -> uint64_t
        {
            if (pitch < 0)
            {
                return static_cast<uint64_t>(-pitch);
            }
            return static_cast<uint64_t>(pitch);
        }();
        if (!locked.pixels().data || rowBytes < uint64_t{ width } * 4 ||
            rowBytes * (height - 1) > static_cast<uint64_t>((std::numeric_limits<ptrdiff_t>::max)()))
        {
            throw std::runtime_error("Invalid game backbuffer row pitch");
        }
        for (UINT y = 0; y < height; ++y)
        {
            const BYTE* source      = locked.pixels().data + static_cast<ptrdiff_t>(int64_t{ y } * pitch);
            BYTE*       destination = bgr.data() + static_cast<size_t>(y) * width * 3;
            for (UINT x = 0; x < width; ++x)
            {
                destination[x * 3]     = source[x * 4];
                destination[x * 3 + 1] = source[x * 4 + 1];
                destination[x * 3 + 2] = source[x * 4 + 2];
            }
        }
        locked.unlock();
    }
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create PNG encoder");
    ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
    check(frame->Initialize(nullptr), "Initialize PNG frame");
    check(frame->SetSize(width, height), "Set PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&format), "Set PNG pixel format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
    {
        throw std::runtime_error("PNG encoder cannot preserve backbuffer colors");
    }
    check(frame->WritePixels(height, width * 3, static_cast<UINT>(bgr.size()), bgr.data()),
          "Encode game screenshot");
    check(frame->Commit(), "Finish PNG frame");
    check(encoder->Commit(), "Finish PNG screenshot");
    return stream;
}

std::filesystem::path writeScreenshot(IStream* stream)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        wchar_t name[128];
        swprintf_s(name, L"TetraMaster_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u.png", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId(), sequence.fetch_add(1));
        const auto path = options.runtimeDir / "logs/screenshots" / name;
        if (!outputPathAllowed(options, path))
        {
            throw std::runtime_error("Screenshot folder is not safe");
        }
        std::filesystem::create_directories(path.parent_path());
        if (!outputPathAllowed(options, path))
        {
            throw std::runtime_error("Screenshot folder is not safe");
        }
        // CREATE_NEW is the final authority: no existing file is overwritten,
        // even if another process creates this name after the path checks.
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            {
                continue;
            }
            check(HRESULT_FROM_WIN32(error), "Create screenshot file");
        }
        try
        {
            LARGE_INTEGER start{};
            check(stream->Seek(start, STREAM_SEEK_SET, nullptr), "Read PNG stream");
            std::array<BYTE, 65536> buffer{};
            for (;;)
            {
                ULONG read = 0;
                check(stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read), "Read PNG stream");
                if (!read)
                {
                    break;
                }
                DWORD written = 0;
                if (!WriteFile(file, buffer.data(), read, &written, nullptr))
                {
                    check(HRESULT_FROM_WIN32(GetLastError()), "Write screenshot file");
                }
                if (written != read)
                {
                    throw std::runtime_error("Incomplete screenshot file write");
                }
            }
            if (!CloseHandle(file))
            {
                file = INVALID_HANDLE_VALUE;
                check(HRESULT_FROM_WIN32(GetLastError()), "Close screenshot file");
            }
            return path;
        }
        catch (...)
        {
            // Only the unique file created by this invocation is removed.
            if (file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file);
            }
            DeleteFileW(path.c_str());
            throw;
        }
    }
    throw std::runtime_error("Could not choose an unused screenshot filename");
}

} // namespace

void requestScreenshot() noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        pending.store(true);
        status = "Screenshot queued for the next game frame.";
    }
    catch (...)
    {
    }
}

std::string screenshotStatus()
{
    std::lock_guard<std::mutex> lock(statusMutex);
    return status;
}

void saveRequestedScreenshot(void* device) noexcept
{
    if (!pending.load())
    {
        return;
    }
    try
    {
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            if (!pending.exchange(false))
            {
                return;
            }
            status = "Saving screenshot...";
        }
        ComScope   com;
        auto       stream  = encodeBackbuffer(device);
        const auto path    = writeScreenshot(stream.Get());
        const auto utf8    = path.u8string();
        const auto message = "Screenshot saved: " +
                             std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        finishStatus(message);
        logf("%s", message.c_str());
    }
    catch (const std::exception& error)
    {
        logf("Screenshot failed: %s", error.what());
        try
        {
            finishStatus(std::string("Screenshot failed: ") + error.what());
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        logf("Screenshot failed: unknown error");
        try
        {
            finishStatus("Screenshot failed: unknown error");
        }
        catch (...)
        {
        }
    }
}

} // namespace tmloader
