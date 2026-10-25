#pragma once

#include "War3MapCore/MapData/TerrainBoundsEditing.h"

#include <cstdint>

namespace War3Map::MapData {

// 相对本笔before西南角的地格半开矩形；扩西/南为负，原canvas为[0,0,W,H]。
// 只扩裁外矩形，原可玩世界范围与重叠内容的世界坐标保持不变。
struct TerrainCanvasRectangle {
    std::int32_t west = 0, south = 0, east = 0, north = 0;
    bool operator==(const TerrainCanvasRectangle&) const = default;
};

struct TerrainResizeIntent {
    std::uint8_t algorithm = 1;
    TerrainCanvasRectangle canvas;
    bool operator==(const TerrainResizeIntent&) const = default;
};

struct TerrainResizeLimits {
    // 沿用结构笔刷硬帽；候选、可逆材料、规则与空间扫描累计预收，只能收紧。
    std::size_t maximumCandidateBytes = 256u * 1024u * 1024u;
    std::size_t maximumWork = 16u * 1024u * 1024u;
};

// 纯数值投影所需规则，不授予来源或保存资格；正式命令只接受实际资源owner。
struct TerrainResizeDimensionRules {
    std::uint32_t minimumSide = 0, maximumSide = 0;
    std::uint32_t sideStep = 32, maximumArea = 0;
};

struct TerrainResizeGeometry {
    TerrainGridSnapshot terrain;
    MapSettings settings;
    TerrainPlayableRectangle playable;
    std::size_t preparationBytes = 0, preparationWork = 0;
};

[[nodiscard]] Status ValidateTerrainResizeIntent(const TerrainResizeIntent& intent);

// 纯候选不认证尺寸配置/CameraMargins来源；不读文件，不处理摆放或授予保存资格。
[[nodiscard]] Result<TerrainResizeGeometry> ProjectTerrainResizeGeometry(
    const TerrainGridSnapshot& source, const MapSettings& settings,
    const std::array<float, 4>& cameraMargins, const TerrainResizeDimensionRules& dimensions,
    const TerrainResizeIntent& intent, const TerrainResizeLimits& limits = {});

} // namespace War3Map::MapData
