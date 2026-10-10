#pragma once

#include "War3MapCore/MapData/MapPlacements.h"

#include <string_view>

namespace War3Map::MapData {

// mode=0本身不是随机对象标记：真实经典固定记录同样保留level=1/class=0。
// 此函数只判断已证配置形式，不授予对象类别、来源、唯一身份或保存资格。
[[nodiscard]] inline bool IsKnownFixedPlacementConfig(const RandomUnitConfig& config) noexcept {
    const auto* defaults = std::get_if<RandomAnyUnit>(&config);
    return std::holds_alternative<std::monostate>(config)
        || (defaults && defaults->level == 1 && defaults->itemClass == 0);
}

// 保留的完整随机占位ID；绝不以rawcode首字母猜单位/建筑/物品类别。
[[nodiscard]] inline bool IsRandomPlacementMarker(std::string_view rawcode) noexcept {
    return rawcode == "uDNR" || rawcode == "bDNR" || rawcode == "iDNR";
}

} // namespace War3Map::MapData
