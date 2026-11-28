#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/PlacementManifest.h"
#include "War3MapCore/Services/ObjectCommands.h"

#include <array>
#include <cstddef>
#include <cstdint>
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

// 模型写明范围检查。游戏 1.27a 只在装饰物模型的写明范围（MODL/SEQS/GEOS，不是顶点）与镜头目标附近的区域
// 相交时才绘制：整个在地图外的范围永远不画；镜头目标在观察点前方几百单位，游戏里验证过的规则是 MODL 与每条
// SEQS 范围都包住观察点 ±512 的立方体（按摆放的朝向与缩放）才画出来，没包住的（例如深埋地下）有被剔掉的风险。
// 检查只读、只提醒；加宽须显式开启。
inline constexpr double PlacementImportDefaultStandpointMargin = 512;
inline constexpr double PlacementImportMaximumStandpointMargin = 1000000;
inline constexpr std::size_t PlacementImportReportedBoundsInstances = 16; // 每条提醒列出的实例序号数

struct PlacementImportBoundsOptions {
    std::optional<std::array<double, 3>> standpoint; // 观察点 P（世界坐标），须在地图范围内
    double margin = PlacementImportDefaultStandpointMargin; // P 周围立方体的半边长：范围须包住 [P−margin, P+margin]
    // 把有问题的模型的全部写明范围加宽到包含每个摆放处的 P±margin（各摆放按自己的轴心、朝向和缩放换算，取并集）。
    // 需要 standpoint；只改导入副本里这些范围字段，作者的源文件不动。
    bool widen = false;
};

enum class PlacementBoundsProblem : std::uint8_t { OffMap = 1, FarFromStandpoint = 2, Both = 3 };

struct PlacementImportBoundsFinding {
    std::size_t instance = 0; // 清单 instances 下标
    std::uint32_t type = 0;   // 清单 types 下标
    PlacementBoundsProblem problem = PlacementBoundsProblem::OffMap;
    std::array<double, 6> box{}; // 摆放后的世界范围：最小 xyz、最大 xyz
    // P 在写明范围里的深度（世界单位）：以 P 为中心、半边长为它的立方体恰好还在 MODL 与每条 SEQS 范围里
    // （按摆放的朝向与缩放）；小于 margin 即为 FarFromStandpoint，负值是 P 在范围外的距离。没给 P 时为 0。
    double clearance = 0;
};

struct PlacementImportBoundsModel {
    std::string archivePath;
    std::size_t placements = 0, offMap = 0, farFromStandpoint = 0;
    bool checked = false;
    std::string uncheckedReason; // checked 为假时的原因
    std::size_t widenedExtents = 0; // 加宽时改写的范围条数；0 表示未改
};

struct PlacementImportBoundsReport {
    std::optional<std::array<double, 3>> standpoint;
    double margin = PlacementImportDefaultStandpointMargin;
    bool widenRequested = false;
    std::size_t checkedInstances = 0, uncheckedInstances = 0;
    std::size_t offMap = 0, farFromStandpoint = 0, both = 0; // 互斥计数：both 不再计入前两项
    std::size_t widenedModels = 0;
    std::vector<PlacementImportBoundsFinding> findings; // 有问题的实例，按清单顺序
    std::vector<PlacementImportBoundsModel> models;     // 装饰物用到的清单内模型文件，按 files 顺序
};

struct PlacementImportTypeResult {
    std::string key;
    MapData::PlacementManifestKind kind = MapData::PlacementManifestKind::Doodad;
    std::string rawcode, base;
    std::size_t instances = 0;
    bool boundsChecked = false;                 // 模型在清单文件里且范围可读（仅装饰物）
    std::size_t offMap = 0, farFromStandpoint = 0, both = 0; // 本类型的范围问题实例数（互斥计数）
};

struct PlacementImportReport {
    std::vector<PlacementImportTypeResult> types;
    std::size_t files = 0, replacedFiles = 0;
    std::size_t doodads = 0, fixedHeightDoodads = 0, units = 0;
    PlacementImportBoundsReport bounds;
    std::vector<Diagnostic> warnings; // 不拦截的提醒，如模型几何体超出游戏上限、写明范围在地图外或离观察点远
};

struct PlacementImportInputs {
    // document 为地图当前物编；standard/schema/documents 与之同一来源。只用到清单里实际出现的类别。
    ObjectCommandContext doodads, units;
    MapData::TerrainHeader terrain; // 世界范围：西南角起 (width-1)×128、(height-1)×128
    std::function<bool(std::string_view)> memberExists; // 可选，仅用于报告替换的已有文件
    // 可选：地面高度，用于贴地装饰物（游戏不用存储的 Z）；没有或取不到时按清单 z 计算。
    std::function<std::optional<float>(double x, double y)> groundHeight;
    PlacementImportBoundsOptions bounds;
};

// 加宽后的文件：files 下标与改写后的完整字节。
struct PlacementImportFileRewrite {
    std::size_t file = 0;
    std::vector<std::byte> bytes;
};

// 导入文件的字节留在清单里，不复制进计划；提交方取 manifest.files，有 fileRewrites 的文件改用其中的字节。
struct PlacementImportPlan {
    std::optional<ObjectCommandPlan> doodadObjects, unitObjects; // before = 输入文档，after = 新建全部类型后
    std::vector<MapData::DoodadPlacement> doodads; // 创建编号由提交时分配
    std::vector<MapData::UnitPlacement> units;
    std::vector<PlacementImportFileRewrite> fileRewrites; // 按 file 升序
    PlacementImportReport report;
};

[[nodiscard]] Result<PlacementImportPlan> PlanPlacementImport(
    const MapData::PlacementManifest& manifest, const PlacementImportInputs& inputs);

// 提交时第 file 个文件实际写入的字节：有加宽结果用加宽结果，否则用清单原字节。
[[nodiscard]] const std::vector<std::byte>& PlacementImportFileBytes(
    const MapData::PlacementManifest& manifest, const PlacementImportPlan& plan, std::size_t file);

} // namespace War3Map::Services
