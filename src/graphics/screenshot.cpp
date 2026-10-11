#include "graphics/screenshot.h"
#include "app/log.h"
#include "app/runtime.h"
#include "graphics/d3d8_abi.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <wil/resource.h>
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
    const std::lock_guard lock(statusMutex);
    // A request queued during encoding keeps its status until its own frame.
    if (!pending.load())
    {
        status = message;
    }
}

void check(HRESULT hr, const char* action)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(std::format("{} (0x{:08X})", action, hex(hr)));
    }
}

struct Image
{
    UINT              width  = 0;
    UINT              height = 0;
    std::vector<BYTE> bgr;
};

// Copies the backbuffer to 24-bit BGR; render-target alpha is unreliable.
auto readBackbuffer(void* device) -> Image
{
    if (!device)
    {
        throw std::runtime_error("Game renderer is unavailable");
    }

    ComPtr<IUnknown> surface;
    check(callMethod(device, d3d8::GetBackBuffer, UINT{ 0 }, DWORD{ 0 }, surface.GetAddressOf()), "Get game backbuffer");
    if (!surface)
    {
        throw std::runtime_error("Game backbuffer is unavailable");
    }

    auto desc = d3d8::SurfaceDesc{};
    check(callMethod(surface.Get(), d3d8::SurfaceGetDesc, &desc), "Read game backbuffer dimensions");
    if (desc.format != d3d8::kFormatA8R8G8B8 && desc.format != d3d8::kFormatX8R8G8B8)
    {
        throw std::runtime_error("Screenshot requires a 32-bit game backbuffer");
    }

    if (!desc.width || !desc.height || desc.width > 16384 || desc.height > 16384)
    {
        throw std::runtime_error("Invalid game backbuffer dimensions");
    }

    auto locked = d3d8::LockedRect{};
    check(callMethod(surface.Get(), d3d8::SurfaceLockRect, &locked, static_cast<const RECT*>(nullptr), d3d8::kLockReadOnly), "Read game backbuffer");
    const auto unlock   = wil::scope_exit([&]
                                          {
                                            callMethod(surface.Get(), d3d8::SurfaceUnlock);
                                          });
    const auto pitch    = int64_t{ locked.pitch };
    const auto rowBytes = static_cast<uint64_t>(std::abs(pitch));
    if (!locked.bits || rowBytes < uint64_t{ desc.width } * 4 ||
        rowBytes * (desc.height - 1) > static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max()))
    {
        throw std::runtime_error("Invalid game backbuffer row pitch");
    }

    auto image = Image{ desc.width, desc.height, std::vector<BYTE>(size_t{ desc.width } * desc.height * 3) };
    for (UINT y = 0; y < image.height; ++y)
    {
        const auto* const source      = locked.bits + static_cast<ptrdiff_t>(int64_t{ y } * pitch);
        auto* const       destination = image.bgr.data() + size_t{ y } * image.width * 3;
        for (UINT x = 0; x < image.width; ++x)
        {
            std::copy_n(source + x * 4, 3, destination + x * 3);
        }
    }

    return image;
}

auto encodePng(const Image& image) -> std::vector<BYTE>
{
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
    check(frame->SetSize(image.width, image.height), "Set PNG dimensions");
    auto format = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&format), "Set PNG pixel format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
    {
        throw std::runtime_error("PNG encoder cannot preserve backbuffer colors");
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* const pixels = const_cast<BYTE*>(image.bgr.data());
    check(frame->WritePixels(image.height, image.width * 3, static_cast<UINT>(image.bgr.size()), pixels), "Encode game screenshot");
    check(frame->Commit(), "Finish PNG frame");
    check(encoder->Commit(), "Finish PNG screenshot");

    auto stat = STATSTG{};
    check(stream->Stat(&stat, STATFLAG_NONAME), "Read PNG stream");
    HGLOBAL memory = nullptr;
    check(GetHGlobalFromStream(stream.Get(), &memory), "Read PNG stream");
    const auto* const bytes  = static_cast<const BYTE*>(GlobalLock(memory));
    const auto        unlock = wil::scope_exit([&]
                                               {
                                            GlobalUnlock(memory);
                                               });
    return { bytes, bytes + stat.cbSize.QuadPart };
}

// Returns false if the file already exists; never overwrites one.
auto writeNewFile(const std::filesystem::path& path, const std::vector<BYTE>& bytes) -> bool
{
    auto file = wil::unique_hfile(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
        {
            return false;
        }

        check(HRESULT_FROM_WIN32(error), "Create screenshot file");
    }

    auto remove  = wil::scope_exit([&]
                                   {
                                      file.reset();
                                      DeleteFileW(path.c_str());
                                   });
    auto written = DWORD{};
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
    {
        check(HRESULT_FROM_WIN32(GetLastError()), "Write screenshot file");
    }

    if (written != bytes.size())
    {
        throw std::runtime_error("Incomplete screenshot file write");
    }

    remove.release();
    return true;
}

auto writeScreenshot(const std::vector<BYTE>& png) -> std::filesystem::path
{
    auto time = SYSTEMTIME{};
    GetLocalTime(&time);
    const auto directory = options.runtimeDir / "logs/screenshots";
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        const auto name = std::format(L"TetraMaster_{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}_{}_{}.png", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId(), sequence.fetch_add(1));
        const auto path = directory / name;
        if (!outputPathAllowed(options, path))
        {
            throw std::runtime_error("Screenshot folder is not safe");
        }

        std::filesystem::create_directories(directory);
        if (writeNewFile(path, png))
        {
            return path;
        }
    }

    throw std::runtime_error("Could not choose an unused screenshot filename");
}

void saveScreenshot(void* device)
{
    // The game thread may already own a COM apartment of another kind.
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (com != RPC_E_CHANGED_MODE)
    {
        check(com, "Initialize PNG encoder");
    }

    const auto uninitialize = wil::scope_exit([com]
                                              {
                                                  if (SUCCEEDED(com))
                                                  {
                                                      CoUninitialize();
                                                  }
                                              });
    const auto path         = writeScreenshot(encodePng(readBackbuffer(device)));
    const auto message      = "Screenshot saved: " + narrow(path.native());
    finishStatus(message);
    logLine("{}", message);
}

} // namespace

void requestScreenshot() noexcept
{
    try
    {
        const std::lock_guard lock(statusMutex);
        pending.store(true);
        status = "Screenshot queued for the next game frame.";
    }
    catch (...)
    {
    }
}

auto screenshotStatus() -> std::string
{
    const std::lock_guard lock(statusMutex);
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
            const std::lock_guard lock(statusMutex);
            if (!pending.exchange(false))
            {
                return;
            }

            status = "Saving screenshot...";
        }

        saveScreenshot(device);
    }
    catch (const std::exception& error)
    {
        logLine("Screenshot failed: {}", error.what());
        try
        {
            finishStatus(std::string("Screenshot failed: ") + error.what());
        }
        catch (...)
        {
        }
    }
}

} // namespace tmloader
