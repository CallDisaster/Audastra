#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/PlacementManifest.h"
#include "War3MapCore/Services/ObjectCommands.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

// 摆放清单的规划：按当前物编新建自定义类型（自动分配 rawcode、写模型字段），生成摆放记录。
// 只读输入，不改任何会话；宿主再分别提交到导入、物编和摆放各自的撤销记录，命令行保存则一次写出。
// 无占地、无声音、无阴影、全地形组可用、单变体的原版装饰物；未指定 base 的装饰物类型从它派生。
inline constexpr std::string_view PlacementImportDefaultDoodadBase = "LOsp";
inline constexpr std::size_t PlacementImportReportedOutside = 32;

struct PlacementImportTypeResult {
    std::string key;
    MapData::PlacementManifestKind kind = MapData::PlacementManifestKind::Doodad;
    std::string rawcode, base;
    std::size_t instances = 0;
};

struct PlacementImportReport {
    std::vector<PlacementImportTypeResult> types;
    std::size_t files = 0, replacedFiles = 0;
    std::size_t doodads = 0, fixedHeightDoodads = 0, units = 0;
};

struct PlacementImportInputs {
    // document 为地图当前物编；standard/schema/documents 与之同一来源。只用到清单里实际出现的类别。
    ObjectCommandContext doodads, units;
    MapData::TerrainHeader terrain; // 世界范围：西南角起 (width-1)×128、(height-1)×128
    std::function<bool(std::string_view)> memberExists; // 可选，仅用于报告替换的已有文件
};

// 导入文件的字节留在清单里，不复制进计划；提交方直接取 manifest.files。
struct PlacementImportPlan {
    std::optional<ObjectCommandPlan> doodadObjects, unitObjects; // before = 输入文档，after = 新建全部类型后
    std::vector<MapData::DoodadPlacement> doodads; // 创建编号由提交时分配
    std::vector<MapData::UnitPlacement> units;
    PlacementImportReport report;
};

[[nodiscard]] Result<PlacementImportPlan> PlanPlacementImport(
    const MapData::PlacementManifest& manifest, const PlacementImportInputs& inputs);

} // namespace War3Map::Services
