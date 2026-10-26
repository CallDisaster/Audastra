#pragma once
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/MapRuntimePackage.h"

namespace War3Map::Services {
struct MapRuntimeSaveState {
    MapRuntimeSettings settings;
    std::string approvedPackageSha256;
    std::string previousIdentity;
    std::string previousScriptSha256;
    std::vector<MemberReplacement> previousMembers;
    std::vector<MemberReplacement> expectedMembers;
    std::shared_ptr<const MapRuntimePackage> package;
    std::string scriptMember;
    std::optional<std::vector<std::byte>> settingsBytes;
    std::string scriptSha256;
    std::vector<std::string> removedMembers;
    bool active = false;
    MapRuntimeSavePurpose purpose = MapRuntimeSavePurpose::Normal;
};
[[nodiscard]] Status PrepareMapRuntimeSave(MapSaveTask& task);
[[nodiscard]] Status FinalizeMapRuntimeSave(MapSaveTask& task);
[[nodiscard]] Status VerifyMapRuntimeSave(const MapSaveTask& task);
}
