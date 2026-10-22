#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"

namespace War3Map::MapData {

// WPM 的格范围比 W3E 最北/最东一排顶点少半开边界；二者不能合并成“无阻挡”。
enum class TerrainPathingState { Unavailable, Outside, Available };

struct TerrainPointSample {
    TerrainStrokePoint point;
    float groundHeight = 0; // 原始地高、悬崖层级及已支持坡道的公共三角采样。
    std::uint32_t vertexX = 0, vertexY = 0;
    TerrainVertex vertex; // 最近顶点的原始字段，不把插值伪装成原始记录。
    std::optional<std::string> groundTexture, cliffTexture; // 无解析表项或悬崖哨兵时为空。
    TerrainPathingState pathingState = TerrainPathingState::Unavailable;
    std::uint32_t pathingX = 0, pathingY = 0;
    std::optional<std::uint8_t> pathingFlags; // 当前静态 WPM 原始位，不等于游戏动态寻路结果。
    std::optional<std::uint8_t> originalPathingFlags;
    std::uint8_t addedPathingRestrictions = 0; // 本工程声明的 0x02/0x04/0x08，包含与来源限制重叠的意图。
};

} // namespace War3Map::MapData
