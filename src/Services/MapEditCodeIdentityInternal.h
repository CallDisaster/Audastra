#pragma once
#include "War3MapCore/Services/MapEditSaveService.h"
namespace War3Map::Services {
[[nodiscard]] Result<Trigger::UiCatalog> ResolveMapEditCatalog(const std::filesystem::path& root,
    const std::optional<std::vector<std::string>>& layers);
[[nodiscard]] Result<MapEditSaveReceipt> SaveTriggerCodeIdentityCandidate(const MapEditSaveRequest& request,
    const MapSaveService::Progress& progress);
}
