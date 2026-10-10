#pragma once

#include "War3MapCore/Trigger/TriggerGraphEditing.h"

namespace War3Map::Trigger::detail {

struct ArgumentReplacementInspection {
    GraphArgumentReplacementGuard guard;
    // 不含保留的根参数；旧根无 index 时恰为待摘除 call 的完整闭包。
    std::size_t removedEcas = 0;
    std::size_t removedArguments = 0;
};

[[nodiscard]] Result<ArgumentReplacementInspection> InspectArgumentReplacement(
    const TriggerDocument& document, const UiCatalog& catalog, StableId argumentId,
    const TriggerGraphEditLimits& limits, const MapObjectReferenceSnapshot* mapObjects = nullptr);

// 新值可以是已验证的数组来源；不把“旧根 index 暂不支持替换”误用于成功回执。
[[nodiscard]] Result<std::string> FingerprintArgumentSubtree(
    const TriggerDocument& document, const UiCatalog& catalog, StableId argumentId,
    const TriggerGraphEditLimits& limits, const MapObjectReferenceSnapshot* mapObjects = nullptr);

} // namespace War3Map::Trigger::detail
