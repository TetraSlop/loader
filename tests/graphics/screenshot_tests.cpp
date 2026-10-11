#include "app/runtime.h"
#include "graphics/d3d8_abi.h"
#include "graphics/screenshot.h"
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <format>
#include <fstream>
#include <sddl.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr int               kSurfacePitch   = 20;
constexpr std::wstring_view kRootPrefix     = L"tmloader-screenshot-test-";
constexpr DWORD             kChildTimeoutMs = 30000;
// The fake surface's two rows as the PNG must hold them: same colors, alpha forced opaque.
const std::vector<DWORD> kOpaquePixels{ 0xff112233, 0xff445566, 0xff778899, 0xff0a20e0, 0xff203040, 0xfff1f2f3 };

bool failScreenshotWrite = false;
bool requestDuringWrite  = false;

void WINAPI fixedScreenshotTime(SYSTEMTIME* time)
{
    *time = { 2026, 9, 0, 27, 12, 34, 56, 789 };
}

auto WINAPI screenshotWrite(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlap) -> BOOL
{
    if (requestDuringWrite)
    {
        requestDuringWrite = false;
        tmloader::requestScreenshot();
    }

    if (failScreenshotWrite)
    {
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }

    return WriteFile(file, data, size, written, overlap);
}

} // namespace

#define GetLocalTime fixedScreenshotTime
#define WriteFile    screenshotWrite
#include "graphics/screenshot.cpp"
#undef WriteFile
#undef GetLocalTime

namespace tmloader
{

Config options;

void writeLog(std::string_view)
{
}

} // namespace tmloader

namespace
{

namespace d3d8 = tmloader::d3d8;
using Microsoft::WRL::ComPtr;
using tmloader::check;
using tmloader::requestScreenshot;
using tmloader::saveRequestedScreenshot;
using tmloader::screenshotStatus;

struct Surface
{
    void**                                     vtable;
    std::array<void*, d3d8::SurfaceUnlock + 1> methods{};
    DWORD                                      format = d3d8::kFormatA8R8G8B8;
    DWORD                                      width  = 3;
    DWORD                                      height = 2;
    int                                        pitch  = kSurfacePitch;
    std::array<BYTE, 2 * kSurfacePitch>        storage{};
    ULONG                                      references = 1;
    unsigned                                   locks      = 0;
    unsigned                                   unlocks    = 0;
    HRESULT                                    descResult = S_OK;
    HRESULT                                    lockResult = S_OK;
    bool                                       nullPixels = false;

    Surface()
    : vtable(methods.data())
    {
        methods[2]                     = reinterpret_cast<void*>(release); // IUnknown::Release
        methods[d3d8::SurfaceGetDesc]  = reinterpret_cast<void*>(description);
        methods[d3d8::SurfaceLockRect] = reinterpret_cast<void*>(lock);
        methods[d3d8::SurfaceUnlock]   = reinterpret_cast<void*>(unlock);
        storage.fill(0xab);
        // BGRA bytes deliberately contain zero and partial alpha values.
        const auto top    = std::array<DWORD, 3>{ 0x00112233, 0x80445566, 0xff778899 };
        const auto bottom = std::array<DWORD, 3>{ 0x010a20e0, 0x10203040, 0x00f1f2f3 };
        std::memcpy(storage.data(), top.data(), sizeof(top));
        std::memcpy(storage.data() + kSurfacePitch, bottom.data(), sizeof(bottom));
    }

    static auto WINAPI release(Surface* self) -> ULONG
    {
        return --self->references;
    }

    static auto WINAPI description(Surface* self, d3d8::SurfaceDesc* output) -> HRESULT
    {
        output->format = self->format;
        output->width  = self->width;
        output->height = self->height;
        return self->descResult;
    }

    static auto WINAPI lock(Surface* self, d3d8::LockedRect* output, const RECT* rect, DWORD flags) -> HRESULT
    {
        REQUIRE(rect == nullptr);
        REQUIRE(flags == d3d8::kLockReadOnly);
        ++self->locks;
        if (FAILED(self->lockResult))
        {
            return self->lockResult;
        }

        output->pitch = self->pitch;
        output->bits  = nullptr;
        if (!self->nullPixels)
        {
            output->bits = self->storage.data();
            if (self->pitch < 0)
            {
                output->bits += kSurfacePitch;
            }
        }

        return S_OK;
    }

    static auto WINAPI unlock(Surface* self) -> HRESULT
    {
        ++self->unlocks;
        return S_OK;
    }
};

struct Device
{
    void**                                     vtable;
    std::array<void*, d3d8::GetBackBuffer + 1> methods{};
    Surface                                    surface;
    unsigned                                   reads  = 0;
    HRESULT                                    result = S_OK;

    Device()
    : vtable(methods.data())
    {
        methods[d3d8::GetBackBuffer] = reinterpret_cast<void*>(backbuffer);
    }

    static auto WINAPI backbuffer(Device* self, UINT index, DWORD type, void** output) -> HRESULT
    {
        REQUIRE(index == 0);
        REQUIRE(type == 0);
        ++self->reads;
        if (FAILED(self->result))
        {
            return self->result;
        }

        ++self->surface.references;
        *output = &self->surface;
        return S_OK;
    }
};

auto files() -> std::set<std::filesystem::path>
{
    std::set<std::filesystem::path> result;
    const auto                      dir = tmloader::options.runtimeDir / "logs/screenshots";
    if (std::filesystem::exists(dir))
    {
        for (const auto& file : std::filesystem::directory_iterator(dir))
        {
            result.insert(file.path());
        }
    }

    return result;
}

auto requestedFileName(unsigned number) -> std::filesystem::path
{
    return tmloader::options.runtimeDir / "logs/screenshots" /
           std::format(L"TetraMaster_20260927_123456_789_{}_{}.png", GetCurrentProcessId(), number);
}

auto decode(const std::filesystem::path& path) -> std::vector<DWORD>
{
    const auto                 com   = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto                 scope = wil::scope_exit([com]
                                                       {
                                           if (SUCCEEDED(com))
                                           {
                                               CoUninitialize();
                                           }
                                                       });
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Test decoder");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder), "Open captured PNG");
    GUID container{};
    check(decoder->GetContainerFormat(&container), "PNG container");
    REQUIRE(IsEqualGUID(container, GUID_ContainerFormatPng));
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "PNG frame");
    UINT width  = 0;
    UINT height = 0;
    check(frame->GetSize(&width, &height), "PNG dimensions");
    REQUIRE(width == 3);
    REQUIRE(height == 2);
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "PNG converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "PNG conversion");
    auto result = std::vector<DWORD>(width * height);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.size() * 4), reinterpret_cast<BYTE*>(result.data())), "PNG pixels");
    return result;
}

auto statusContains(const std::string_view text) -> bool
{
    return screenshotStatus().find(text) != std::string::npos;
}

auto statusReportsSave() -> bool
{
    return screenshotStatus().starts_with("Screenshot saved:");
}

void requireCaptureFails(Device& device, const char* expected, unsigned locks, unsigned unlocks)
{
    const auto before = files();
    requestScreenshot();
    saveRequestedScreenshot(&device);
    REQUIRE(files() == before);
    REQUIRE(statusContains(expected));
    REQUIRE(device.surface.locks == locks);
    REQUIRE(device.surface.unlocks == unlocks);
    REQUIRE(device.surface.references == 1);
}

// Points the screenshot code at an empty runtime directory and clears whatever an earlier case left queued.
struct RuntimeDir
{
    std::filesystem::path root;

    RuntimeDir()
    {
        const auto parent = std::filesystem::temp_directory_path();
        // The accented character keeps non-ASCII runtime paths covered.
        root = parent / (std::wstring(kRootPrefix) + L"\u00e9-" + std::to_wstring(GetCurrentProcessId()) +
                         L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(root));
        // The destructor deletes this tree, so be sure of where it is.
        REQUIRE(tmloader::pathIsWithin(root, parent));
        REQUIRE(root.filename().wstring().find(kRootPrefix) == 0);
        tmloader::options.runtimeDir = root;
        tmloader::options.game       = root / "TM_native.dll";
        tmloader::options.core       = root / "polcore_native.dll";
        tmloader::pending.store(false);
        tmloader::status.clear();
        failScreenshotWrite = false;
        requestDuringWrite  = false;
    }

    RuntimeDir(const RuntimeDir&)                    = delete;
    auto operator=(const RuntimeDir&) -> RuntimeDir& = delete;
    RuntimeDir(RuntimeDir&&)                         = delete;
    auto operator=(RuntimeDir&&) -> RuntimeDir&      = delete;

    ~RuntimeDir()
    {
        auto error = std::error_code{};
        std::filesystem::remove_all(root, error);
    }
};

void labelFixture(const std::filesystem::path& path, const wchar_t* sddl)
{
    REQUIRE(std::filesystem::create_directory(path));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr));
    PACL label     = nullptr;
    BOOL present   = FALSE;
    BOOL defaulted = FALSE;
    REQUIRE(GetSecurityDescriptorSacl(descriptor, &present, &label, &defaulted));
    REQUIRE(present);
    auto       mutablePath = path.wstring();
    const auto result      = SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, label);
    LocalFree(descriptor);
    REQUIRE(result == ERROR_SUCCESS);
}

void lowIntegrityCapture(const std::filesystem::path& fixture)
{
    // This is a disposable child process. Match the loader's token label,
    // then initialize WIC for the first time only after the label is lowered.
    HANDLE token = nullptr;
    PSID   sid   = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &token));
    REQUIRE(ConvertStringSidToSidW(L"S-1-16-4096", &sid));
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid        = sid;
    REQUIRE(SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid)));
    LocalFree(sid);
    std::array<BYTE, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> actual{};
    DWORD                                                                   size = 0;
    REQUIRE(GetTokenInformation(token, TokenIntegrityLevel, actual.data(), static_cast<DWORD>(actual.size()), &size));
    const auto integrity = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(actual.data())->Label.Sid;
    REQUIRE(*GetSidSubAuthority(integrity, *GetSidSubAuthorityCount(integrity) - 1) == SECURITY_MANDATORY_LOW_RID);
    CloseHandle(token);

    // The medium-labeled control proves that low-write isolation is effective.
    const auto forbidden = CreateFileW((fixture / "medium/control.png").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto error     = GetLastError();
    REQUIRE(forbidden == INVALID_HANDLE_VALUE);
    REQUIRE(error == ERROR_ACCESS_DENIED);
    tmloader::options.runtimeDir = fixture / "low";
    tmloader::options.game       = tmloader::options.runtimeDir / "TM_native.dll";
    tmloader::options.core       = tmloader::options.runtimeDir / "polcore_native.dll";
    Device device;
    requestScreenshot();
    saveRequestedScreenshot(&device);
    REQUIRE(statusReportsSave());
    REQUIRE(files().size() == 1);
    REQUIRE(device.surface.references == 1);
    REQUIRE(device.surface.unlocks == 1);
    REQUIRE(decode(*files().begin()) == kOpaquePixels);
}

void verifyLowIntegrityCapture(const std::filesystem::path& fixture)
{
    labelFixture(fixture / "low", L"S:(ML;OICI;NW;;;LW)");
    labelFixture(fixture / "medium", L"S:(ML;OICI;NW;;;ME)");
    wchar_t executable[32768]{};
    REQUIRE(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))));
    auto         command = L"\"" + std::wstring(executable) + L"\" --low-integrity \"" + fixture.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    const auto wait = WaitForSingleObject(process.hProcess, kChildTimeoutMs);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }

    DWORD exitCode = 1;
    REQUIRE(GetExitCodeProcess(process.hProcess, &exitCode));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REQUIRE(wait == WAIT_OBJECT_0);
    REQUIRE(exitCode == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture / "medium/control.png"));
}

} // namespace

TEST_CASE("Nothing is captured without a request", "[screenshot]")
{
    const RuntimeDir runtime;
    Device           device;
    saveRequestedScreenshot(nullptr);
    saveRequestedScreenshot(&device);
    REQUIRE(device.reads == 0);
    REQUIRE(files().empty());
    REQUIRE(screenshotStatus().empty());
}

TEST_CASE("A request is captured on the next frame and consumed once", "[screenshot]")
{
    const RuntimeDir runtime;
    Device           device;
    const auto       first = tmloader::sequence.load();
    std::filesystem::create_directories(runtime.root / "logs/screenshots");
    const auto existing = requestedFileName(first);
    {
        std::ofstream output(existing, std::ios::binary);
        output << "Never replace me";
    }

    requestScreenshot();
    requestScreenshot();
    REQUIRE(statusContains("queued"));

    saveRequestedScreenshot(&device);
    REQUIRE(device.reads == 1);
    REQUIRE(device.surface.references == 1);
    REQUIRE(device.surface.locks == 1);
    REQUIRE(device.surface.unlocks == 1);

    SECTION("an existing file with the chosen name is kept")
    {
        REQUIRE(files().size() == 2);
        REQUIRE(std::filesystem::exists(requestedFileName(first + 1)));
        std::ifstream     input(existing, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), {});
        REQUIRE(content == "Never replace me");
    }

    SECTION("the PNG holds the surface pixels and the status names the file")
    {
        REQUIRE(decode(requestedFileName(first + 1)) == kOpaquePixels);
        const auto expectedPath = requestedFileName(first + 1).u8string();
        REQUIRE(statusContains(std::string(reinterpret_cast<const char*>(expectedPath.data()), expectedPath.size())));
    }

    SECTION("later frames do not capture again")
    {
        saveRequestedScreenshot(&device);
        REQUIRE(device.reads == 1);
        REQUIRE(files().size() == 2);
    }
}

TEST_CASE("A bottom-up X8R8G8B8 surface is captured the right way up", "[screenshot]")
{
    const RuntimeDir runtime;
    Device           device;
    const auto       first = tmloader::sequence.load();
    device.surface.format  = d3d8::kFormatX8R8G8B8;
    device.surface.pitch   = -kSurfacePitch;
    requestScreenshot();
    saveRequestedScreenshot(&device);

    auto reversed = kOpaquePixels;
    std::swap_ranges(reversed.begin(), reversed.begin() + 3, reversed.begin() + 3);
    REQUIRE(decode(requestedFileName(first)) == reversed);
    REQUIRE(device.surface.references == 1);
    REQUIRE(device.surface.unlocks == 1);
}

TEST_CASE("A failed capture writes nothing and releases the surface", "[screenshot]")
{
    const RuntimeDir runtime;
    Device           device;

    SECTION("16-bit surface")
    {
        device.surface.format = 23; // D3DFMT_R5G6B5
        requireCaptureFails(device, "32-bit", 0, 0);
    }

    SECTION("zero-width surface")
    {
        device.surface.width = 0;
        requireCaptureFails(device, "dimensions", 0, 0);
    }

    SECTION("surface description unavailable")
    {
        device.surface.descResult = E_FAIL;
        requireCaptureFails(device, "dimensions", 0, 0);
    }

    SECTION("surface cannot be locked")
    {
        device.surface.lockResult = E_FAIL;
        requireCaptureFails(device, "Read game backbuffer", 1, 0);
    }

    // A lock that succeeded is always released, whatever goes wrong afterwards.
    SECTION("pitch shorter than a row")
    {
        device.surface.pitch = 8;
        requireCaptureFails(device, "pitch", 1, 1);
    }

    SECTION("lock returns no pixels")
    {
        device.surface.nullPixels = true;
        requireCaptureFails(device, "pitch", 1, 1);
    }

    SECTION("back buffer unavailable")
    {
        device.result = E_FAIL;
        requireCaptureFails(device, "Get game backbuffer", 0, 0);
    }

    SECTION("output would alias a configured input file")
    {
        tmloader::options.game = requestedFileName(tmloader::sequence.load());
        requireCaptureFails(device, "not safe", 1, 1);
    }

    SECTION("disk write fails")
    {
        failScreenshotWrite = true;
        requireCaptureFails(device, "Write screenshot file", 1, 1);
        failScreenshotWrite = false;
    }
}

TEST_CASE("A request without a renderer reports it as unavailable", "[screenshot]")
{
    const RuntimeDir runtime;
    requestScreenshot();
    saveRequestedScreenshot(nullptr);
    REQUIRE(statusContains("unavailable"));
}

TEST_CASE("The caller's COM apartment is left as it was", "[screenshot]")
{
    const RuntimeDir runtime;
    Device           device;
    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    requestScreenshot();
    saveRequestedScreenshot(&device);
    // A context token exists only while the thread is still inside an apartment.
    ULONG_PTR token = 0;
    REQUIRE(SUCCEEDED(CoGetContextToken(&token)));
    REQUIRE(token != 0);
    CoUninitialize();
}

TEST_CASE("A request made while a capture is being written is kept", "[screenshot]")
{
    const RuntimeDir runtime;
    Device           device;

    SECTION("when the write succeeds")
    {
        requestDuringWrite = true;
        requestScreenshot();
        saveRequestedScreenshot(&device);
        REQUIRE(device.reads == 1);
        REQUIRE(statusContains("queued"));

        saveRequestedScreenshot(&device);
        REQUIRE(device.reads == 2);
        REQUIRE(statusReportsSave());

        saveRequestedScreenshot(&device);
        REQUIRE(device.reads == 2);
    }

    SECTION("when the write fails")
    {
        requestDuringWrite  = true;
        failScreenshotWrite = true;
        requestScreenshot();
        saveRequestedScreenshot(&device);
        REQUIRE(statusContains("queued"));

        failScreenshotWrite = false;
        saveRequestedScreenshot(&device);
        REQUIRE(statusReportsSave());
    }
}

TEST_CASE("A low-integrity process can capture into a low-integrity folder", "[screenshot]")
{
    const RuntimeDir runtime;
    verifyLowIntegrityCapture(runtime.root);
}

namespace
{

std::filesystem::path childFixture;

} // namespace

// wmain selects this case by name when it is relaunched with --low-integrity; keep the two in step.
TEST_CASE("Low-integrity screenshot child", "[.screenshot-child]")
{
    REQUIRE_FALSE(childFixture.empty());
    lowIntegrityCapture(childFixture);
}

auto wmain(int argc, wchar_t** argv) -> int
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--low-integrity")
    {
        childFixture = argv[2];
        const wchar_t* childArgs[]{ argv[0], L"Low-integrity screenshot child" };
        return Catch::Session().run(2, childArgs);
    }

    return Catch::Session().run(argc, argv);
}
