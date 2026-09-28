#include "app/runtime.h"
#include "graphics/screenshot.h"
#include <aclapi.h>
#include <algorithm>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <sddl.h>
#include <set>

static void WINAPI fixedScreenshotTime(SYSTEMTIME* time)
{
    *time = { 2026, 9, 0, 27, 12, 34, 56, 789 };
}

static bool failScreenshotWrite = false;
static bool requestDuringWrite  = false;

static auto WINAPI screenshotWrite(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlap) -> BOOL
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

#define GetLocalTime fixedScreenshotTime
#define WriteFile    screenshotWrite
#include "graphics/screenshot.cpp"
#undef WriteFile
#undef GetLocalTime

namespace tmloader
{

Config options;

void logf(const char*, ...)
{
}

} // namespace tmloader

namespace
{

using namespace tmloader;
using Microsoft::WRL::ComPtr;

struct LockedPixels
{
    INT   pitch;
    BYTE* data;
};

struct Surface
{
    void**                vtable;
    std::array<void*, 11> methods{};
    DWORD                 format = 21, width = 3, height = 2;
    int                   pitch = 20;
    std::array<BYTE, 40>  storage{};
    ULONG                 references = 1;
    unsigned              locks = 0, unlocks = 0;
    HRESULT               descResult = S_OK, lockResult = S_OK;
    bool                  nullPixels = false;

    Surface()
    : vtable(methods.data())
    {
        methods[2]  = reinterpret_cast<void*>(release);
        methods[8]  = reinterpret_cast<void*>(description);
        methods[9]  = reinterpret_cast<void*>(lock);
        methods[10] = reinterpret_cast<void*>(unlock);
        storage.fill(0xab);
        // BGRA bytes deliberately contain zero and partial alpha values.
        const DWORD top[]{ 0x00112233, 0x80445566, 0xff778899 };
        const DWORD bottom[]{ 0x010a20e0, 0x10203040, 0x00f1f2f3 };
        std::memcpy(storage.data(), top, sizeof(top));
        std::memcpy(storage.data() + 20, bottom, sizeof(bottom));
    }

    static auto WINAPI release(Surface* self) -> ULONG
    {
        return --self->references;
    }

    static auto WINAPI description(Surface* self, DWORD* output) -> HRESULT
    {
        output[0] = self->format;
        output[6] = self->width;
        output[7] = self->height;
        return self->descResult;
    }

    static auto WINAPI lock(Surface* self, LockedPixels* output, const RECT* rect, DWORD flags) -> HRESULT
    {
        REQUIRE((!rect && flags == 0x10));
        ++self->locks;
        if (FAILED(self->lockResult))
            return self->lockResult;
        output->pitch = self->pitch;
        output->data  = self->nullPixels ? nullptr : self->storage.data() + (self->pitch < 0 ? 20 : 0);
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
    void**                vtable;
    std::array<void*, 17> methods{};
    Surface               surface;
    unsigned              reads  = 0;
    HRESULT               result = S_OK;

    Device()
    : vtable(methods.data())
    {
        methods[16] = reinterpret_cast<void*>(backbuffer);
    }

    static auto WINAPI backbuffer(Device* self, UINT index, DWORD type, void** output) -> HRESULT
    {
        REQUIRE((index == 0 && type == 0));
        ++self->reads;
        if (FAILED(self->result))
            return self->result;
        ++self->surface.references;
        *output = &self->surface;
        return S_OK;
    }
};

auto files() -> std::set<std::filesystem::path>
{
    std::set<std::filesystem::path> result;
    const auto                      dir = options.runtimeDir / "logs/screenshots";
    if (std::filesystem::exists(dir))
        for (const auto& file : std::filesystem::directory_iterator(dir))
            result.insert(file.path());
    return result;
}

auto requestedFileName(unsigned number) -> std::filesystem::path
{
    wchar_t name[128];
    swprintf_s(name, L"TetraMaster_20260927_123456_789_%lu_%u.png", GetCurrentProcessId(), number);
    return options.runtimeDir / "logs/screenshots" / name;
}

auto decode(const std::filesystem::path& path) -> std::vector<DWORD>
{
    ComScope                   com;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Test decoder");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder), "Open captured PNG");
    GUID container{};
    check(decoder->GetContainerFormat(&container), "PNG container");
    REQUIRE(IsEqualGUID(container, GUID_ContainerFormatPng));
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "PNG frame");
    UINT width = 0, height = 0;
    check(frame->GetSize(&width, &height), "PNG dimensions");
    REQUIRE((width == 3 && height == 2));
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "PNG converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "PNG conversion");
    std::vector<DWORD> result(width * height);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.size() * 4), reinterpret_cast<BYTE*>(result.data())), "PNG pixels");
    return result;
}

void fails(Device& device, const char* expected, unsigned locks, unsigned unlocks)
{
    const auto before = files();
    requestScreenshot();
    saveRequestedScreenshot(&device);
    REQUIRE(files() == before);
    REQUIRE(screenshotStatus().find(expected) != std::string::npos);
    REQUIRE((device.surface.locks == locks && device.surface.unlocks == unlocks));
    REQUIRE(device.surface.references == 1);
}

void labelFixture(const std::filesystem::path& path, const wchar_t* sddl)
{
    REQUIRE(std::filesystem::create_directory(path));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr));
    PACL label   = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    REQUIRE((GetSecurityDescriptorSacl(descriptor, &present, &label, &defaulted) && present));
    auto        mutablePath = path.wstring();
    const DWORD result      = SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, label);
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
    REQUIRE((forbidden == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED));
    options.runtimeDir = fixture / "low";
    options.game       = options.runtimeDir / "TM_native.dll";
    options.core       = options.runtimeDir / "polcore_native.dll";
    Device device;
    requestScreenshot();
    saveRequestedScreenshot(&device);
    REQUIRE(screenshotStatus().find("Screenshot saved:") == 0);
    REQUIRE((files().size() == 1 && device.surface.references == 1 && device.surface.unlocks == 1));
    const std::vector<DWORD> expected{ 0xff112233, 0xff445566, 0xff778899, 0xff0a20e0, 0xff203040, 0xfff1f2f3 };
    REQUIRE(decode(*files().begin()) == expected);
}

void verifyLowIntegrityCapture(const std::filesystem::path& fixture)
{
    labelFixture(fixture / "low", L"S:(ML;OICI;NW;;;LW)");
    labelFixture(fixture / "medium", L"S:(ML;OICI;NW;;;ME)");
    wchar_t executable[32768]{};
    REQUIRE(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --low-integrity \"" + fixture.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 1;
    REQUIRE(GetExitCodeProcess(process.hProcess, &exitCode));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REQUIRE((wait == WAIT_OBJECT_0 && exitCode == 0));
    REQUIRE(!std::filesystem::exists(fixture / "medium/control.png"));
}

} // namespace

TEST_CASE("Screenshots preserve pixels and safe output lifecycle", "[screenshot]")
{
    using namespace tmloader;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto parent = std::filesystem::temp_directory_path();
    const auto root   = parent / (L"tmloader-screenshot-test-\u00e9-" + std::to_wstring(GetCurrentProcessId()) +
                                  L"-" + std::to_wstring(GetTickCount64()));
    REQUIRE(std::filesystem::create_directory(root));
    options.runtimeDir = root;
    options.game       = root / "TM_native.dll";
    options.core       = root / "polcore_native.dll";

    Device device;
    saveRequestedScreenshot(nullptr);
    saveRequestedScreenshot(&device);
    REQUIRE((device.reads == 0 && files().empty() && screenshotStatus().empty()));

    std::filesystem::create_directories(root / "logs/screenshots");
    const auto existing = requestedFileName(0);
    {
        std::ofstream output(existing, std::ios::binary);
        output << "Never replace me";
    }
    requestScreenshot();
    requestScreenshot();
    REQUIRE(screenshotStatus().find("queued") != std::string::npos);
    saveRequestedScreenshot(&device);
    REQUIRE((device.reads == 1 && device.surface.references == 1));
    REQUIRE((device.surface.locks == 1 && device.surface.unlocks == 1));
    REQUIRE((files().size() == 2 && std::filesystem::exists(requestedFileName(1))));
    {
        std::ifstream     input(existing, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), {});
        REQUIRE(content == "Never replace me");
    }
    const std::vector<DWORD> expected{ 0xff112233, 0xff445566, 0xff778899, 0xff0a20e0, 0xff203040, 0xfff1f2f3 };
    REQUIRE(decode(requestedFileName(1)) == expected);
    const auto expectedPath = requestedFileName(1).u8string();
    REQUIRE(screenshotStatus().find(std::string(reinterpret_cast<const char*>(expectedPath.data()),
                                                expectedPath.size())) != std::string::npos);
    saveRequestedScreenshot(&device);
    REQUIRE((device.reads == 1 && files().size() == 2)); // A request is consumed once.

    device.surface.format = 22;
    device.surface.pitch  = -20;
    requestScreenshot();
    saveRequestedScreenshot(&device);
    auto reversed = expected;
    std::swap_ranges(reversed.begin(), reversed.begin() + 3, reversed.begin() + 3);
    REQUIRE(decode(requestedFileName(2)) == reversed);
    REQUIRE((device.surface.references == 1 && device.surface.unlocks == 2));

    Device bad;
    bad.surface.format = 23;
    fails(bad, "32-bit", 0, 0);
    bad.surface.format = 21;
    bad.surface.width  = 0;
    fails(bad, "dimensions", 0, 0);
    bad.surface.width      = 3;
    bad.surface.descResult = E_FAIL;
    fails(bad, "dimensions", 0, 0);
    bad.surface.descResult = S_OK;
    bad.surface.lockResult = E_FAIL;
    fails(bad, "Read game backbuffer", 1, 0);
    bad.surface.lockResult = S_OK;
    bad.surface.pitch      = 8;
    fails(bad, "pitch", 2, 1); // Successful lock always gets unlocked on errors.
    bad.surface.pitch      = 20;
    bad.surface.nullPixels = true;
    fails(bad, "pitch", 3, 2);
    bad.surface.nullPixels = false;
    bad.result             = E_FAIL;
    fails(bad, "Get game backbuffer", 3, 2);
    bad.result = S_OK;

    // Path validation must refuse an output that aliases a configured input.
    options.game = requestedFileName(sequence.load());
    fails(bad, "not safe", 4, 3);
    options.game        = root / "TM_native.dll";
    failScreenshotWrite = true;
    fails(bad, "Write screenshot file", 5, 4);
    failScreenshotWrite = false;
    requestScreenshot();
    saveRequestedScreenshot(nullptr);
    REQUIRE(screenshotStatus().find("unavailable") != std::string::npos);

    // Verify an existing COM apartment is neither changed nor uninitialized.
    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    requestScreenshot();
    saveRequestedScreenshot(&device);
    ULONG_PTR token = 0;
    REQUIRE((SUCCEEDED(CoGetContextToken(&token)) && token != 0));
    CoUninitialize();
    // A newer request must survive completion of the in-progress screenshot.
    requestDuringWrite   = true;
    const unsigned reads = device.reads;
    requestScreenshot();
    saveRequestedScreenshot(&device);
    REQUIRE((device.reads == reads + 1 && screenshotStatus().find("queued") != std::string::npos));
    saveRequestedScreenshot(&device);
    REQUIRE((device.reads == reads + 2 && screenshotStatus().find("Screenshot saved:") == 0));
    saveRequestedScreenshot(&device);
    REQUIRE(device.reads == reads + 2);
    // The same rule holds if the older request fails while another is queued.
    requestDuringWrite  = true;
    failScreenshotWrite = true;
    requestScreenshot();
    saveRequestedScreenshot(&device);
    REQUIRE(screenshotStatus().find("queued") != std::string::npos);
    failScreenshotWrite = false;
    saveRequestedScreenshot(&device);
    REQUIRE(screenshotStatus().find("Screenshot saved:") == 0);

    verifyLowIntegrityCapture(root);
    REQUIRE((pathIsWithin(root, parent) && root.filename().wstring().find(L"tmloader-screenshot-test-") == 0));
    std::filesystem::remove_all(root);
    puts("Screenshot pixels, queue ordering, low-integrity WIC/file writes, collision and cleanup tests passed");
}

namespace
{

std::filesystem::path childFixture;

}

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
