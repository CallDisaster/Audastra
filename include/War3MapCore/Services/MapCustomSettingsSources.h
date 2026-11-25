#pragma once

#include "War3MapCore/Services/MapCustomSettingsEditing.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/ObjectCommandSources.h"

namespace War3Map::Services {

// 物编来源与通用物编命令共用 CaptureObjectCommandSources。
using MapCustomSettingsObjectOverlay = ObjectDocumentOverlay;
[[nodiscard]] Result<std::vector<std::string>> MapCustomSettingsInputNames();
[[nodiscard]] Result<MapCustomSettingsValidation> CaptureMapCustomSettingsValidation(
    const ObjectCatalogSources& sources, MapCustomSettingsObjectOverlay overlay = {},
    const AuxiliaryReferenceContext& references = {});

} // namespace War3Map::Services
