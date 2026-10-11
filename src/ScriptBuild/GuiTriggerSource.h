#pragma once

#include "War3MapCore/Script/MapScript.h"

namespace War3Map::Trigger { class TriggerTypeSystem; }

namespace War3Map::Script::detail {

// 代码恢复的本次解析已准备类型目录。回生成仍走完整验证，只在调用期间借用同一目录。
[[nodiscard]] Result<GuiTriggerSource> GenerateGuiTriggerSourceWithTypes(const MapScriptInputs& inputs,
    StableId triggerId, const Trigger::TriggerTypeSystem& types);

} // namespace War3Map::Script::detail
