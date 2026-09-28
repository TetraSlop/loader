#include "app/runtime.h"
#include <aclapi.h>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <wil/resource.h>

namespace
{

auto integrityRid(const std::filesystem::path& path) -> DWORD
{
    PACL                                   label = nullptr;
    wil::unique_hlocal_security_descriptor descriptor;
    REQUIRE(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, &label, &descriptor) == ERROR_SUCCESS);
    if (!label || !label->AceCount)
    {
        return SECURITY_MANDATORY_MEDIUM_RID;
    }

    void* ace = nullptr;
    REQUIRE(GetAce(label, 0, &ace));
    const auto sid = reinterpret_cast<PSID>(&static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace)->SidStart);
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

} // namespace

TEST_CASE("Writable runtime folders are labeled for the low-integrity game", "[runtime]")
{
    const auto root = std::filesystem::temp_directory_path() / ("tmloader-runtime-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "pub/home00");
    std::ofstream(root / "pub/home00/option.bin") << "save";
    std::ofstream(root / "TM_native.dll") << "dll";

    tmloader::allowLowIntegrityWrites(root);

    for (const auto* const name : { "pub", "pub/home00/option.bin", "logs", "settings" })
    {
        CAPTURE(name);
        REQUIRE(integrityRid(root / name) == SECURITY_MANDATORY_LOW_RID);
    }

    REQUIRE(integrityRid(root / "TM_native.dll") == SECURITY_MANDATORY_MEDIUM_RID);
    std::filesystem::remove_all(root);
}
