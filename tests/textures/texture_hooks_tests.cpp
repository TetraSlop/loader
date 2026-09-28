#include "textures/texture_hooks.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace tmloader;

namespace
{

constexpr DWORD                imageSize = 0x1ba000;
constexpr DWORD                uploadRva = 0x1b9aa4;
constexpr std::array<DWORD, 7> sites{
    0x17ba39, 0x17c061, 0x17c1f5, 0x17c393, 0x17c528, 0x17ccfa, 0x17ce91
};
unsigned calls = 0;

auto WINAPI replacement(void* device, const void* source, UINT bytes, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD key, void* info, void* palette, void** output) -> HRESULT
{
    REQUIRE((device == reinterpret_cast<void*>(1) && source == reinterpret_cast<void*>(2)));
    REQUIRE((bytes == 3 && width == 4 && height == 5 && levels == 6 && usage == 7));
    REQUIRE((format == 8 && pool == 9 && filter == 10 && mipFilter == 11 && key == 12));
    REQUIRE((info == reinterpret_cast<void*>(13) && palette == reinterpret_cast<void*>(14)));
    *output = reinterpret_cast<void*>(15);
    ++calls;
    return 12345;
}

void initialize(BYTE* image)
{
    std::memset(image, 0xcc, imageSize);
    const auto prologue = std::array<BYTE, 8>{ 0x55, 0x8b, 0xec, 0xff, 0x75, 0x40, 0x6a, 0x03 };
    const auto end      = std::array<BYTE, 3>{ 0xc2, 0x3c, 0x00 };
    std::memcpy(image + uploadRva, prologue.data(), prologue.size());
    std::memcpy(image + 0x1b9ae1, end.data(), end.size());
    for (const DWORD rva : sites)
    {
        auto*      entry = image + rva - 48;
        const auto frame = std::array<BYTE, 3>{ 0x55, 0x8b, 0xec };
        std::memcpy(entry, frame.data(), frame.size());
        entry += frame.size();
        // Synthetic stdcall caller: forward each of the fifteen arguments.
        for (BYTE offset = 0x40; offset >= 8; offset -= 4)
        {
            *entry++ = 0xff;
            *entry++ = 0x75;
            *entry++ = offset;
        }

        REQUIRE(entry == image + rva);
        *entry++            = 0xe8;
        const auto relative = uploadRva - rva - 5;
        std::memcpy(entry, &relative, sizeof(relative));
        entry += sizeof(relative);
        const auto finish = std::array<BYTE, 4>{ 0x5d, 0xc2, 0x3c, 0x00 };
        std::memcpy(entry, finish.data(), finish.size());
    }
}

struct Image
{
    BYTE* data = static_cast<BYTE*>(VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    Image()
    {
        REQUIRE(data);
        initialize(data);
    }

    ~Image()
    {
        VirtualFree(data, 0, MEM_RELEASE);
    }

    Image(const Image&)                    = delete;
    auto operator=(const Image&) -> Image& = delete;

    void seal()
    {
        DWORD old{};
        REQUIRE(VirtualProtect(data, imageSize, PAGE_EXECUTE_READ, &old));
        REQUIRE(FlushInstructionCache(GetCurrentProcess(), data, imageSize));
    }
};

auto protection(const BYTE* address) -> DWORD
{
    MEMORY_BASIC_INFORMATION memory{};
    REQUIRE(VirtualQuery(address, &memory, sizeof(memory)));
    return memory.Protect;
}

template <class F>
void rejected(F&& action)
{
    REQUIRE_THROWS_AS(action(), std::exception);
}

void successfulDispatch()
{
    Image image;
    image.seal();
    const auto original = installTextureCallHooks(image.data, replacement);
    REQUIRE(original == reinterpret_cast<TextureUploadFn>(image.data + uploadRva));
    for (const DWORD rva : sites)
    {
        REQUIRE(protection(image.data + rva) == PAGE_EXECUTE_READ);
        std::uint32_t displacement{};
        std::memcpy(&displacement, image.data + rva + 1, sizeof(displacement));
        REQUIRE(reinterpret_cast<std::uintptr_t>(image.data + rva + 5) + displacement ==
                reinterpret_cast<std::uintptr_t>(replacement));
        const auto caller = reinterpret_cast<TextureUploadFn>(image.data + rva - 48);
        void*      output = nullptr;
        REQUIRE(caller(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, reinterpret_cast<void*>(13), reinterpret_cast<void*>(14), &output) == 12345);
        REQUIRE(output == reinterpret_cast<void*>(15));
    }

    REQUIRE(calls == sites.size());
    // A second installation is rejected rather than creating recursive dispatch.
    rejected([&]
             {
                 installTextureCallHooks(image.data, replacement);
             });
}

void unsupportedCodeIsUntouched()
{
    auto corrupt = std::vector<DWORD>{ uploadRva, 0x1b9ae1 };
    for (const DWORD rva : sites)
    {
        corrupt.push_back(rva + 1);
    }

    for (const DWORD rva : corrupt)
    {
        Image image;
        image.data[rva] ^= 1;
        const auto before = std::vector<BYTE>(image.data, image.data + imageSize);
        image.seal();
        rejected([&]
                 {
                     installTextureCallHooks(image.data, replacement);
                 });
        REQUIRE(std::memcmp(image.data, before.data(), imageSize) == 0);
        for (const DWORD site : sites)
        {
            REQUIRE(protection(image.data + site) == PAGE_EXECUTE_READ);
        }
    }

    rejected([&]
             {
                 installTextureCallHooks(nullptr, replacement);
             });
    Image image;
    rejected([&]
             {
                 installTextureCallHooks(image.data, nullptr);
             });
    DWORD old{};
    REQUIRE(VirtualProtect(image.data + 0x17c000, 0x1000, PAGE_NOACCESS, &old));
    const auto originalCall = std::array<BYTE, 5>{ 0xe8, 0x66, 0xe0, 0x03, 0x00 };
    rejected([&]
             {
                 installTextureCallHooks(image.data, replacement);
             });
    REQUIRE(std::memcmp(image.data + sites[0], originalCall.data(), originalCall.size()) == 0);
}

void protectionFailureIsUntouched()
{
    // This mapping supports reading/writing but not executable protection. A
    // read-only view remains readable for guards; the patch acquisition fails.
    const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, imageSize, nullptr);
    REQUIRE(mapping);
    auto* const writable = static_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, imageSize));
    REQUIRE(writable);
    initialize(writable);
    const auto before = std::vector<BYTE>(writable, writable + imageSize);
    REQUIRE(UnmapViewOfFile(writable));
    auto* const readonly = static_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, imageSize));
    REQUIRE((readonly && protection(readonly + sites[0]) == PAGE_READONLY));
    rejected([&]
             {
                 installTextureCallHooks(readonly, replacement);
             });
    REQUIRE(std::memcmp(readonly, before.data(), imageSize) == 0);
    for (const DWORD site : sites)
    {
        REQUIRE(protection(readonly + site) == PAGE_READONLY);
    }

    REQUIRE(UnmapViewOfFile(readonly));
    REQUIRE(CloseHandle(mapping));
}

} // namespace

TEST_CASE("Texture hooks preserve native stdcall dispatch", "[texture_hooks]")
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    successfulDispatch();
}

TEST_CASE("Texture hooks reject unsupported code without modifying it", "[texture_hooks]")
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    unsupportedCodeIsUntouched();
}

TEST_CASE("Texture hooks leave read-only mappings intact", "[texture_hooks]")
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    protectionFailureIsUntouched();
}
