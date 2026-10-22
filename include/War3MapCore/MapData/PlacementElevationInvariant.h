#pragma once

#include "War3MapCore/MapData/MapPlacements.h"

namespace War3Map::MapData {

// 只核对经典摆放成员是否仅改变普通记录的 Z；不授予地形或通行保存资格。
// 未改成员逐字保留，不要求未知旧布局可编辑。改变成员则须完整解析并通过
// 原 writer 重写核对：XY、行顺序、全部其它字段和特殊摆放尾段必须原字相同。
// maximumInputBytes 同时限制两个输入；调用方仍须核源身份、资源与保存修订。
[[nodiscard]] Status VerifyUnitPlacementElevationOnly(std::span<const std::byte> before,
    std::span<const std::byte> after, std::size_t maximumInputBytes = 64 * 1024 * 1024);
[[nodiscard]] Status VerifyDoodadPlacementElevationOnly(std::span<const std::byte> before,
    std::span<const std::byte> after, std::size_t maximumInputBytes = 64 * 1024 * 1024);

} // namespace War3Map::MapData
