#pragma once

#include "War3MapCore/Services/MapCustomSettingsEditing.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"

namespace War3Map::Services {

// 当前工程物编覆盖必须由宿主实际文档复制；空指针表示从同一物理来源捕获，不能伪造缺席。
using MapCustomSettingsObjectOverlay = std::array<std::shared_ptr<const ObjectData::LniObjectDocument>, 7>;
[[nodiscard]] Result<std::vector<std::string>> MapCustomSettingsInputNames();
[[nodiscard]] Result<MapCustomSettingsValidation> CaptureMapCustomSettingsValidation(
    const ObjectCatalogSources& sources, MapCustomSettingsObjectOverlay overlay = {},
    const AuxiliaryReferenceContext& references = {});

} // namespace War3Map::Services
