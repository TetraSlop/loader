#include "texture_pack.h"
#include "texture_replacements.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unknwn.h>

using namespace tmloader;
namespace fs = std::filesystem;

namespace tmloader
{

void logf(const char*, ...)
{
}

} // namespace tmloader

// Structural PNG fixtures: decoding is deliberately left to the upload callback.
// This checks bounded matching independently of any PNG codec.
std::vector<uint8_t> png(unsigned width, unsigned height)
{
    std::vector<uint8_t> bytes{ 137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    for (unsigned value : { width, height })
        for (int shift : { 24, 16, 8, 0 })
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    bytes.insert(bytes.end(), { 8, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0 });
    return bytes;
}

void write(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

template <class F>
void rejects(F fn)
{
    REQUIRE_THROWS_AS(fn(), std::exception);
}

struct FakeTexture
{
    void**    table;
    unsigned  width, height, references = 1;
    IUnknown* attachment = nullptr;
};

bool failAttachment = false;

ULONG WINAPI addRef(FakeTexture* self)
{
    return ++self->references;
}

ULONG WINAPI release(FakeTexture* self)
{
    return --self->references;
}

HRESULT WINAPI description(FakeTexture* self, UINT level, DWORD* desc)
{
    REQUIRE(level == 0);
    desc[0] = 21;
    desc[6] = self->width;
    desc[7] = self->height;
    return S_OK;
}

HRESULT WINAPI privateData(FakeTexture* self, REFGUID, const void* object, DWORD size, DWORD flags)
{
    REQUIRE((size == sizeof(void*) && flags == 1));
    if (failAttachment)
        return E_FAIL;
    if (self->attachment)
        self->attachment->Release();
    self->attachment = const_cast<IUnknown*>(static_cast<const IUnknown*>(object));
    self->attachment->AddRef();
    return S_OK;
}

HRESULT WINAPI getPrivateData(FakeTexture* self, REFGUID, void* object, DWORD* size)
{
    REQUIRE(*size == sizeof(void*));
    if (!self->attachment)
        return E_FAIL;
    *static_cast<IUnknown**>(object) = self->attachment;
    self->attachment->AddRef();
    return S_OK;
}

void clearAttachment(FakeTexture& texture)
{
    if (texture.attachment)
        texture.attachment->Release();
    texture.attachment = nullptr;
}

struct FakeDevice
{
    void**       table;
    FakeTexture* bound = nullptr;
};

HRESULT WINAPI getTexture(FakeDevice* self, UINT stage, FakeTexture** output)
{
    REQUIRE(stage == 0);
    *output = self->bound;
    if (*output)
        addRef(*output);
    return S_OK;
}

HRESULT WINAPI setTexture(FakeDevice* self, UINT stage, FakeTexture* texture)
{
    REQUIRE(stage == 0);
    if (texture)
        addRef(texture);
    if (self->bound)
        release(self->bound);
    self->bound = texture;
    return S_OK;
}

void*       vtable[19]{};
FakeTexture original{ vtable, 128, 128 }, replacement{ vtable, 512, 512 };
unsigned    uploads      = 0;
bool        failOriginal = false, failHd = false;

HRESULT WINAPI upload(void*, const void*, UINT, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD color, void* info, void* palette, void** output)
{
    ++uploads;
    REQUIRE((levels == 1 && usage == 0 && format == 21 && filter == 0x80001 && mipFilter == 0xffffffff));
    REQUIRE((!color && !info && !palette));
    if (width == 0xffffffff)
    {
        REQUIRE((height == 0xffffffff && pool == 0));
        *output = failOriginal ? nullptr : &original;
        return failOriginal ? E_FAIL : S_OK;
    }
    REQUIRE((width == 512 && height == 512 && pool == 1)); // Not 288x384: padding must also scale.
    *output = failHd ? nullptr : &replacement;
    return failHd ? E_FAIL : S_OK;
}

HRESULT invoke(const std::vector<uint8_t>& bytes, void** output)
{
    return uploadTextureWithReplacement(nullptr, bytes.data(), static_cast<UINT>(bytes.size()), 0xffffffff, 0xffffffff, 1, 0, 21, 0, 0x80001, 0xffffffff, 0, nullptr, nullptr, output);
}

void checkSelection(TextureCategory category)
{
    void* table[62]{};
    table[60] = reinterpret_cast<void*>(getTexture);
    table[61] = reinterpret_cast<void*>(setTexture);
    FakeDevice device{ table };
    setTexture(&device, 0, &original);
    for (bool artwork : { false, true })
        for (bool fonts : { false, true })
        {
            {
                TextureDrawBinding binding(&device, artwork, fonts);
                const bool         enabled = category == TextureCategory::font ? fonts : artwork;
                REQUIRE(device.bound == (enabled ? &replacement : &original));
            }
            REQUIRE(device.bound == &original);
        }
    for (bool enabled : { false, true })
    {
        {
            TextureDrawBinding combined(&device, enabled);
            REQUIRE(device.bound == (enabled ? &replacement : &original));
        }
        REQUIRE(device.bound == &original);
    }
    setTexture(&device, 0, nullptr);
    REQUIRE((original.references == 1 && replacement.references == 1));
}

TEST_CASE("Texture packs validate sources and retain replacement ownership", "[texture_pack]")
{
    const auto root = fs::current_path() / "texture-pack-fixture";
    REQUIRE(!fs::exists(root));
    fs::create_directory(root);
    auto source = png(72, 96), hd = png(288, 384);
    auto record = nlohmann::json{
        { "id", "test/card" }, { "file", "card.png" }, { "source_sha256", sha256Bytes(source.data(), source.size()) }, { "sha256", sha256Bytes(hd.data(), hd.size()) }, { "logical_size", { 72, 96 } }, { "physical_size", { 288, 384 } }
    };
    const auto manifest = root / "pack.json";
    auto       save     = [&](const nlohmann::json& value)
    {
        std::ofstream out(manifest);
        out << nlohmann::json{ { "version", 1 }, { "textures", nlohmann::json::array({ value }) } };
    };
    write(root / "card.png", hd);
    save(record);
    TexturePack pack;
    pack.load(manifest);
    REQUIRE(pack.size() == 1);
    auto* match = pack.match(source.data(), source.size());
    REQUIRE((match && match->scale == 4 && match->category == TextureCategory::artwork && pack.read(*match) == hd));
    for (unsigned pad = 0; pad <= 15; ++pad)
    {
        auto aligned = source;
        aligned.insert(aligned.end(), pad, 0xcd);
        REQUIRE(pack.match(aligned.data(), aligned.size()) == match);
    }
    auto wrong = source;
    wrong[25] ^= 1;
    REQUIRE(!pack.match(wrong.data(), wrong.size()));
    REQUIRE(!pngInfo(source.data(), 20).bytes);
    wrong    = source;
    wrong[8] = 255;
    REQUIRE(!pngInfo(wrong.data(), wrong.size()).bytes);
    for (const auto* path : { "../card.png", "C:/card.png", "card.png:stream", "/card.png" })
    {
        auto bad    = record;
        bad["file"] = path;
        save(bad);
        rejects([&]
                {
                    pack.load(manifest);
                });
        REQUIRE(pack.size() == 1); // A failed reload leaves the original index usable.
    }
    for (const auto& sizes : { nlohmann::json{ 288, 383 }, nlohmann::json{ 0, 384 }, nlohmann::json{ 8193, 8193 }, nlohmann::json{ 288 } })
    {
        auto bad             = record;
        bad["physical_size"] = sizes;
        save(bad);
        rejects([&]
                {
                    pack.load(manifest);
                });
    }
    // Classify every verified atlas, without treating arbitrary embedded text
    // or similar-looking asset names as font data. No new manifest field needed.
    for (unsigned index = 0; index < 42; ++index)
    {
        auto font  = record;
        font["id"] = "gW000/0090/font_" + std::string(index < 10 ? "0" : "") + std::to_string(index);
        save(font);
        pack.load(manifest);
        REQUIRE(pack.match(source.data(), source.size())->category == TextureCategory::font);
    }
    for (const auto* id : { "gW000/0090/font_42", "gW000/0090/font_0", "gW000/0090/font_000", "gW000/0090/font_01.png", "gW000/0090/font_1a", "gW000/0090/font_-1", "gW000/0092/font_00", "other/font_00", "gW000/0090/window", "gW201/1000/ttl_menu" })
    {
        auto artwork        = record;
        artwork["id"]       = id;
        artwork["category"] = "font";
        save(artwork);
        pack.load(manifest);
        REQUIRE(pack.match(source.data(), source.size())->category == TextureCategory::artwork);
    }
    {
        auto alias  = record;
        alias["id"] = "gW000/0090/font_00";
        std::ofstream out(manifest);
        out << nlohmann::json{ { "version", 1 }, { "textures", { record, alias } } };
        out.close();
        rejects([&]
                {
                    pack.load(manifest);
                }); // Source-hash aliases cannot disagree about category.
    }
    save(record);
    vtable[1]  = reinterpret_cast<void*>(addRef);
    vtable[2]  = reinterpret_cast<void*>(release);
    vtable[4]  = reinterpret_cast<void*>(privateData);
    vtable[5]  = reinterpret_cast<void*>(getPrivateData);
    vtable[14] = reinterpret_cast<void*>(description);
    configureTextureReplacements(manifest, upload);
    void* out = nullptr;
    REQUIRE((invoke(source, &out) == S_OK && out == &original && original.attachment));
    REQUIRE((uploads == 2 && replacement.references == 1)); // Attachment owns the one remaining HD ref.
    REQUIRE((original.width == 128 && original.height == 128));
    checkSelection(TextureCategory::artwork);
    clearAttachment(original);
    REQUIRE(replacement.references == 0);
    for (const auto* id : { "gW000/0090/font_00", "gW201/1000/ttl_menu" })
    {
        auto selected  = record;
        selected["id"] = id;
        save(selected);
        configureTextureReplacements(manifest, upload);
        replacement.references = 1;
        REQUIRE((invoke(source, &out) == S_OK && original.attachment));
        checkSelection(id == std::string("gW000/0090/font_00") ? TextureCategory::font : TextureCategory::artwork);
        clearAttachment(original);
        REQUIRE(replacement.references == 0);
    }
    save(record);
    configureTextureReplacements(manifest, upload);
    replacement.references = 1;
    failAttachment         = true;
    REQUIRE((invoke(source, &out) == S_OK && out == &original && !original.attachment));
    REQUIRE(replacement.references == 0); // Failed private-data attachment releases its companion.
    failAttachment = false;
    uploads        = 0;
    failHd         = true;
    REQUIRE((invoke(source, &out) == S_OK && out == &original && !original.attachment && uploads == 2));
    failHd       = false;
    failOriginal = true;
    uploads      = 0;
    REQUIRE((FAILED(invoke(source, &out)) && !out && uploads == 1));
    failOriginal = false;
    write(root / "card.png", png(284, 384));
    uploads = 0;
    REQUIRE((invoke(source, &out) == S_OK && out == &original && !original.attachment && uploads == 1));
    fs::remove(root / "card.png");
    uploads = 0;
    REQUIRE((invoke(source, &out) == S_OK && out == &original && uploads == 1));
    fs::remove(manifest);
    fs::remove(root);
    std::cout << "PASS: source alignment, category classification, independent font/artwork selection, padded upload and original fallback\n";
}

TEST_CASE("Texture manifests retain UTF-8 filenames", "[texture_pack]")
{
    const auto root = fs::current_path() / "texture-pack-utf8-fixture";
    REQUIRE_FALSE(fs::exists(root));
    REQUIRE(fs::create_directory(root));
    const auto source        = png(72, 96);
    const auto hd            = png(288, 384);
    const auto filename      = std::filesystem::path(u8"carte-\u00e9-\u30c6.png");
    const auto utf8          = filename.u8string();
    const auto filenameBytes = std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    write(root / filename, hd);
    const auto manifest = root / "pack.json";
    {
        std::ofstream output(manifest);
        output << nlohmann::json{ { "version", 1 }, { "textures", { { { "id", "test/card" }, { "file", filenameBytes }, { "source_sha256", sha256Bytes(source.data(), source.size()) }, { "sha256", sha256Bytes(hd.data(), hd.size()) }, { "logical_size", { 72, 96 } }, { "physical_size", { 288, 384 } } } } } };
    }
    TexturePack pack;
    pack.load(manifest);
    const auto* match = pack.match(source.data(), source.size());
    REQUIRE(match);
    REQUIRE(match->file.filename() == filename);
    REQUIRE(pack.read(*match) == hd);
    fs::remove_all(root);
}
