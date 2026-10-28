#pragma once
#include "War3MapCore/Services/MapCustomSettingsEditing.h"

namespace War3Map::Services {
// before/after为空分别表示新增/删除。引用检查在每条私有候选上执行，发布前再执行。
[[nodiscard]] Status CheckRandomSettingsReferences(const MapDocument& document,
    MapCustomSettingsSection section, const MapCustomSettingsValue* before,
    const MapCustomSettingsValue* after, const MapCustomSettingsValidation& validation);
}
