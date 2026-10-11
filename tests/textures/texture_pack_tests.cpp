#include "config/config.h"
#include "graphics/d3d8_abi.h"
#include "textures/texture_pack.h"
#include "textures/texture_replacements.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unknwn.h>
#include <utility>

using namespace tmloader;
namespace fs = std::filesystem;

namespace tmloader
{

void writeLog(std::string_view)
{
}

} // namespace tmloader

namespace
{

// D3DX_DEFAULT: the game lets D3DX pick the size and mip filter of its originals.
constexpr UINT  kD3dxDefault  = 0xffffffff;
constexpr DWORD kUploadFilter = 0x80001;
// The 72x96 card is padded to a power of two on the GPU.
constexpr unsigned kOriginalExtent = 128;
constexpr unsigned kHdExtent       = 512;

// Structural PNG fixtures: decoding is deliberately left to the upload callback.
// This checks bounded matching independently of any PNG codec.
auto png(const unsigned width, const unsigned height) -> std::vector<uint8_t>
{
    auto bytes = std::vector<uint8_t>{ 137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    for (const unsigned value : { width, height })
    {
        for (const int shift : { 24, 16, 8, 0 })
        {
            bytes.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

    bytes.insert(bytes.end(), { 8, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0 });
    return bytes;
}

void write(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A pack directory holding one 4x card and the manifest that describes it.
struct PackFixture
{
    explicit PackFixture(const fs::path& name)
    : root(fs::current_path() / name)
    {
        REQUIRE_FALSE(fs::exists(root));
        fs::create_directory(root);
        write(root / "card.png", hd);
        save(record);
    }

    ~PackFixture()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    PackFixture(const PackFixture&)                    = delete;
    auto operator=(const PackFixture&) -> PackFixture& = delete;
    PackFixture(PackFixture&&)                         = delete;
    auto operator=(PackFixture&&) -> PackFixture&      = delete;

    void save(const nlohmann::json& texture) const
    {
        std::ofstream out(manifest);
        out << nlohmann::json{ { "version", 1 }, { "textures", nlohmann::json::array({ texture }) } };
    }

    const fs::path             root;
    const std::vector<uint8_t> source = png(72, 96);
    const std::vector<uint8_t> hd     = png(288, 384);
    const nlohmann::json       record = nlohmann::json{
        { "id", "test/card" }, { "file", "card.png" }, { "source_sha256", sha256Bytes(source.data(), source.size()) }, { "sha256", sha256Bytes(hd.data(), hd.size()) }, { "logical_size", { 72, 96 } }, { "physical_size", { 288, 384 } }
    };
    const fs::path manifest = root / "pack.json";
};

struct FakeTexture
{
    void**    table{};
    unsigned  width{};
    unsigned  height{};
    unsigned  references = 1;
    IUnknown* attachment = nullptr;
};

bool failAttachment = false;

auto WINAPI addRef(FakeTexture* self) -> ULONG
{
    return ++self->references;
}

auto WINAPI release(FakeTexture* self) -> ULONG
{
    return --self->references;
}

auto WINAPI description(FakeTexture* self, UINT level, d3d8::SurfaceDesc* desc) -> HRESULT
{
    REQUIRE(level == 0);
    desc->format = d3d8::kFormatA8R8G8B8;
    desc->width  = self->width;
    desc->height = self->height;
    return S_OK;
}

auto WINAPI privateData(FakeTexture* self, REFGUID, const void* object, DWORD size, DWORD flags) -> HRESULT
{
    REQUIRE(size == sizeof(void*));
    REQUIRE(flags == d3d8::kPrivateDataObject);
    if (failAttachment)
    {
        return E_FAIL;
    }

    if (self->attachment)
    {
        self->attachment->Release();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    self->attachment = const_cast<IUnknown*>(static_cast<const IUnknown*>(object));
    self->attachment->AddRef();
    return S_OK;
}

auto WINAPI getPrivateData(FakeTexture* self, REFGUID, void* object, DWORD* size) -> HRESULT
{
    REQUIRE(*size == sizeof(void*));
    if (!self->attachment)
    {
        return E_FAIL;
    }

    *static_cast<IUnknown**>(object) = self->attachment;
    self->attachment->AddRef();
    return S_OK;
}

void clearAttachment(FakeTexture& texture)
{
    if (texture.attachment)
    {
        texture.attachment->Release();
    }

    texture.attachment = nullptr;
}

struct FakeDevice
{
    void**       table{};
    FakeTexture* bound = nullptr;
};

auto WINAPI getTexture(FakeDevice* self, UINT stage, FakeTexture** output) -> HRESULT
{
    REQUIRE(stage == 0);
    *output = self->bound;
    if (*output)
    {
        addRef(*output);
    }

    return S_OK;
}

auto WINAPI setTexture(FakeDevice* self, UINT stage, FakeTexture* texture) -> HRESULT
{
    REQUIRE(stage == 0);
    if (texture)
    {
        addRef(texture);
    }

    if (self->bound)
    {
        release(self->bound);
    }

    self->bound = texture;
    return S_OK;
}

auto        vtable = std::array<void*, d3d8::GetLevelDesc + 1>{};
FakeTexture original{ vtable.data(), kOriginalExtent, kOriginalExtent };
FakeTexture replacement{ vtable.data(), kHdExtent, kHdExtent };
unsigned    uploads      = 0;
bool        failOriginal = false;
bool        failHd       = false;

auto WINAPI upload(void*, const void*, UINT, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD color, void* info, void* palette, void** output) -> HRESULT
{
    ++uploads;
    REQUIRE(levels == 1);
    REQUIRE(usage == 0);
    REQUIRE(format == d3d8::kFormatA8R8G8B8);
    REQUIRE(filter == kUploadFilter);
    REQUIRE(mipFilter == kD3dxDefault);
    REQUIRE(color == 0);
    REQUIRE(info == nullptr);
    REQUIRE(palette == nullptr);
    if (width == kD3dxDefault)
    {
        REQUIRE(height == kD3dxDefault);
        REQUIRE(pool == 0);
        if (failOriginal)
        {
            *output = nullptr;
            return E_FAIL;
        }

        *output = &original;
        return S_OK;
    }

    // Not 288x384: padding must also scale.
    REQUIRE(width == kHdExtent);
    REQUIRE(height == kHdExtent);
    REQUIRE(pool == d3d8::kPoolManaged);
    if (failHd)
    {
        *output = nullptr;
        return E_FAIL;
    }

    *output = &replacement;
    return S_OK;
}

// The fakes are file-level, so every case starts them from a known state.
void resetFakes()
{
    vtable[1]                    = reinterpret_cast<void*>(addRef);  // IUnknown::AddRef
    vtable[2]                    = reinterpret_cast<void*>(release); // IUnknown::Release
    vtable[d3d8::SetPrivateData] = reinterpret_cast<void*>(privateData);
    vtable[d3d8::GetPrivateData] = reinterpret_cast<void*>(getPrivateData);
    vtable[d3d8::GetLevelDesc]   = reinterpret_cast<void*>(description);
    clearAttachment(original);
    original.references    = 1;
    replacement.references = 1;
    uploads                = 0;
    failAttachment         = false;
    failOriginal           = false;
    failHd                 = false;
}

auto invoke(const std::vector<uint8_t>& bytes, void** output) -> HRESULT
{
    return uploadTextureWithReplacement(nullptr, bytes.data(), static_cast<UINT>(bytes.size()), kD3dxDefault, kD3dxDefault, 1, 0, d3d8::kFormatA8R8G8B8, 0, kUploadFilter, kD3dxDefault, 0, nullptr, nullptr, output);
}

auto selected(const bool enabled) -> FakeTexture*
{
    if (enabled)
    {
        return &replacement;
    }

    return &original;
}

void checkSelection(const TextureCategory category)
{
    auto table              = std::array<void*, d3d8::SetTexture + 1>{};
    table[d3d8::GetTexture] = reinterpret_cast<void*>(getTexture);
    table[d3d8::SetTexture] = reinterpret_cast<void*>(setTexture);
    FakeDevice device{ table.data() };
    setTexture(&device, 0, &original);
    for (const bool artwork : { false, true })
    {
        for (const bool fonts : { false, true })
        {
            {
                const TextureDrawBinding binding(&device, artwork, fonts);
                const auto               enabled = [&]() -> bool
                {
                    if (category == TextureCategory::font)
                    {
                        return fonts;
                    }

                    return artwork;
                }();
                REQUIRE(device.bound == selected(enabled));
            }

            REQUIRE(device.bound == &original);
        }
    }

    for (const bool enabled : { false, true })
    {
        {
            const TextureDrawBinding combined(&device, enabled, enabled);
            REQUIRE(device.bound == selected(enabled));
        }

        REQUIRE(device.bound == &original);
    }

    setTexture(&device, 0, nullptr);
    REQUIRE(original.references == 1);
    REQUIRE(replacement.references == 1);
}

} // namespace

TEST_CASE("Texture pack matches an original PNG by its hash", "[texture_pack]")
{
    const PackFixture fixture("texture-pack-match-fixture");
    const auto&       source = fixture.source;
    TexturePack       pack;
    pack.load(fixture.manifest);
    REQUIRE(pack.size() == 1);
    const auto* const match = pack.match(source.data(), source.size());
    REQUIRE(match != nullptr);

    SECTION("the asset describes the HD file")
    {
        REQUIRE(match->scale == 4);
        REQUIRE(match->category == TextureCategory::artwork);
        REQUIRE(pack.read(*match) == fixture.hd);
    }

    SECTION("archive alignment padding after IEND is ignored")
    {
        for (unsigned pad = 0; pad <= 15; ++pad)
        {
            auto aligned = source;
            aligned.insert(aligned.end(), pad, 0xcd);
            REQUIRE(pack.match(aligned.data(), aligned.size()) == match);
        }
    }

    SECTION("a PNG with different content does not match")
    {
        auto wrong = source;
        wrong[25] ^= 1;
        REQUIRE(pack.match(wrong.data(), wrong.size()) == nullptr);
    }
}

TEST_CASE("PNG parsing rejects truncated and malformed headers", "[texture_pack]")
{
    const auto source = png(72, 96);
    REQUIRE(pngInfo(source.data(), 20).bytes == 0);

    auto wrong = source;
    wrong[8]   = 255; // IHDR chunk length
    REQUIRE(pngInfo(wrong.data(), wrong.size()).bytes == 0);
}

TEST_CASE("Texture pack rejects invalid manifests", "[texture_pack]")
{
    const PackFixture fixture("texture-pack-manifest-fixture");
    TexturePack       pack;
    pack.load(fixture.manifest);

    SECTION("file paths that leave the pack directory")
    {
        for (const auto* path : { "../card.png", "C:/card.png", "card.png:stream", "/card.png" })
        {
            auto bad    = fixture.record;
            bad["file"] = path;
            fixture.save(bad);
            REQUIRE_THROWS_AS(pack.load(fixture.manifest), std::exception);
            REQUIRE(pack.size() == 1); // A failed reload leaves the original index usable.
        }
    }

    SECTION("physical sizes that are not a uniform scale of the logical size")
    {
        for (const auto& sizes : { nlohmann::json{ 288, 383 }, nlohmann::json{ 0, 384 }, nlohmann::json{ 8193, 8193 }, nlohmann::json{ 288 } })
        {
            auto bad             = fixture.record;
            bad["physical_size"] = sizes;
            fixture.save(bad);
            REQUIRE_THROWS_AS(pack.load(fixture.manifest), std::exception);
        }
    }

    SECTION("source-hash aliases that disagree about category")
    {
        auto alias  = fixture.record;
        alias["id"] = "gW000/0090/font_00";
        {
            std::ofstream out(fixture.manifest);
            out << nlohmann::json{ { "version", 1 }, { "textures", { fixture.record, alias } } };
        }

        REQUIRE_THROWS_AS(pack.load(fixture.manifest), std::exception);
    }
}

TEST_CASE("Only the bitmap font atlases are categorised as fonts", "[texture_pack]")
{
    const PackFixture fixture("texture-pack-category-fixture");
    const auto&       source = fixture.source;
    TexturePack       pack;

    SECTION("all 42 atlases")
    {
        for (unsigned index = 0; index < 42; ++index)
        {
            auto font  = fixture.record;
            font["id"] = std::format("gW000/0090/font_{:02}", index);
            fixture.save(font);
            pack.load(fixture.manifest);
            REQUIRE(pack.match(source.data(), source.size())->category == TextureCategory::font);
        }
    }

    // Similar-looking ids stay artwork, and the manifest cannot opt an asset in.
    SECTION("lookalike ids and a manifest category field")
    {
        for (const auto* id : { "gW000/0090/font_42", "gW000/0090/font_0", "gW000/0090/font_000", "gW000/0090/font_01.png", "gW000/0090/font_1a", "gW000/0090/font_-1", "gW000/0092/font_00", "other/font_00", "gW000/0090/window", "gW201/1000/ttl_menu" })
        {
            auto artwork        = fixture.record;
            artwork["id"]       = id;
            artwork["category"] = "font";
            fixture.save(artwork);
            pack.load(fixture.manifest);
            REQUIRE(pack.match(source.data(), source.size())->category == TextureCategory::artwork);
        }
    }
}

TEST_CASE("Texture upload attaches the HD replacement to the original", "[texture_pack]")
{
    const PackFixture fixture("texture-upload-attach-fixture");
    resetFakes();
    void* out = nullptr;

    SECTION("the game receives its original texture unchanged")
    {
        configureTextureReplacements(fixture.manifest, upload);
        REQUIRE(invoke(fixture.source, &out) == S_OK);
        REQUIRE(out == &original);
        REQUIRE(original.attachment != nullptr);
        REQUIRE(uploads == 2);
        REQUIRE(replacement.references == 1); // Attachment owns the one remaining HD ref.
        REQUIRE(original.width == kOriginalExtent);
        REQUIRE(original.height == kOriginalExtent);
        checkSelection(TextureCategory::artwork);
        clearAttachment(original);
        REQUIRE(replacement.references == 0);
    }

    SECTION("the draw binding follows the asset's category")
    {
        for (const auto& [id, category] : { std::pair<const char*, TextureCategory>{ "gW000/0090/font_00", TextureCategory::font }, { "gW201/1000/ttl_menu", TextureCategory::artwork } })
        {
            auto asset  = fixture.record;
            asset["id"] = id;
            fixture.save(asset);
            configureTextureReplacements(fixture.manifest, upload);
            replacement.references = 1;
            REQUIRE(invoke(fixture.source, &out) == S_OK);
            REQUIRE(original.attachment != nullptr);
            checkSelection(category);
            clearAttachment(original);
            REQUIRE(replacement.references == 0);
        }
    }
}

TEST_CASE("Texture upload falls back to the original alone", "[texture_pack]")
{
    const PackFixture fixture("texture-upload-fallback-fixture");
    resetFakes();
    configureTextureReplacements(fixture.manifest, upload);
    void* out = nullptr;

    SECTION("when the replacement cannot be attached")
    {
        failAttachment = true;
        REQUIRE(invoke(fixture.source, &out) == S_OK);
        REQUIRE(out == &original);
        REQUIRE(original.attachment == nullptr);
        REQUIRE(replacement.references == 0); // Failed private-data attachment releases its companion.
    }

    SECTION("when the HD upload fails")
    {
        failHd = true;
        REQUIRE(invoke(fixture.source, &out) == S_OK);
        REQUIRE(out == &original);
        REQUIRE(original.attachment == nullptr);
        REQUIRE(uploads == 2);
    }

    SECTION("when the HD file no longer matches the manifest")
    {
        write(fixture.root / "card.png", png(284, 384));
        REQUIRE(invoke(fixture.source, &out) == S_OK);
        REQUIRE(out == &original);
        REQUIRE(original.attachment == nullptr);
        REQUIRE(uploads == 1);
    }

    SECTION("when the HD file is missing")
    {
        fs::remove(fixture.root / "card.png");
        REQUIRE(invoke(fixture.source, &out) == S_OK);
        REQUIRE(out == &original);
        REQUIRE(uploads == 1);
    }
}

TEST_CASE("Texture upload passes a failed original upload through", "[texture_pack]")
{
    const PackFixture fixture("texture-upload-failure-fixture");
    resetFakes();
    configureTextureReplacements(fixture.manifest, upload);
    void* out    = &original;
    failOriginal = true;
    REQUIRE(FAILED(invoke(fixture.source, &out)));
    REQUIRE(out == nullptr);
    REQUIRE(uploads == 1);
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
    const auto* const match = pack.match(source.data(), source.size());
    REQUIRE(match);
    REQUIRE(match->file.filename() == filename);
    REQUIRE(pack.read(*match) == hd);
    fs::remove_all(root);
}
