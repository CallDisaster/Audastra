#pragma once

#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/WorldModelScene.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Render {

inline constexpr std::string_view kTerrainCliffModelFailureContextCode = "TERRAIN_CLIFF_MODEL_FAILURE_CONTEXT";

// 一种悬崖（CliffTypes.slk 一行）：模型目录（如 Cliffs / CityCliffs）、坡道模型目录（如 CliffTrans）
// 与替换贴图 11 使用的贴图。坡道目录为空时该类型的坡道边按普通悬崖处理。
struct CliffTypeSource {
    std::string modelDirectory;
    std::string texturePath;
    std::string rampDirectory;
    std::string groundTile;
    std::string upperTile;
};

struct TerrainGroundSelection {
    std::vector<std::uint8_t> indices; // 与原顶点同序；仅派生绘制索引，不改 W3E/palette
    std::size_t ordinaryCliffVertices = 0, derivedVertices = 0, changedVertices = 0;
    std::size_t rampNeighborhoodVertices = 0, retainedFlatVertices = 0;
    std::size_t unknownCliffVertices = 0, missingPairedGroundVertices = 0;
};

// 1.27a 普通悬崖子集：3×3 顶点邻域无 ramp，首个相邻异层格的最低层用 groundTile，其余
// 用 upperTile（未提供则回退 groundTile）。坡道、平层内部标志、NOTH 和缺 palette 项保持 raw
// 并报告覆盖；不增加 palette、不修改来源。UI 与离屏预览共用，返回值拥有其派生索引。
[[nodiscard]] Result<TerrainGroundSelection> ResolveTerrainGroundSelection(
    const MapData::TerrainGridSnapshot& terrain, std::span<const std::optional<CliffTypeSource>> cliffTypes);

struct TerrainCliffs {
    std::vector<BakedModelMesh> meshes;     // 按贴图路径及 TEXS sampler 分组、每块 ≤ 65536 顶点
    std::vector<std::uint8_t> coveredTiles; // 与 tile 同序；1 = 已放置悬崖模型，地面应隐藏
    std::size_t cliffTiles = 0;             // 四角层级不同、且不全是坡道角的格（需要模型）
    std::size_t placedTiles = 0;            // 上述格中已被悬崖或坡道边模型覆盖的
    std::size_t rampPieces = 0;             // 已放置的坡道边模型（每个覆盖两格）
    std::size_t rampSurfaceTiles = 0;       // 四角都是坡道的层级过渡格：坡道面，由地面按贴图绘制
    std::size_t tooHighTiles = 0;           // 层级差超过 2，原版没有对应模型
    std::size_t unknownTypeTiles = 0;       // 悬崖类型索引无效或该类型没有定义
    std::size_t missingModelTiles = 0;      // 所有候选变体均明确回执 RESOURCE_NOT_FOUND
    std::vector<Diagnostic> warnings;
};

// 原版规则：四角（西南、西北、东北、东南）相对最低层级的差记为 A/B/C，模型为
// Doodads\Terrain\<目录>\<目录><四个字母><变体>.mdx，变体取西南角的悬崖变体，缺文件时逐级退到 0。
// 模型在局部 x∈[−128,0]、y∈[0,128]，放在格子东侧边；z 从最低层级起算，顶点再按四角地面起伏
// （去掉层级后的高度）双线性抬升，因此悬崖贴合地面。普通悬崖材质按 SW/SE/NW/NE 取首个非15，
// 全15时有界搜索5×5再退slot0；坡道仍保留当前预览选择规则，与形状字母顺序无关。
// 只读取模型，不修改地形。只有单条明确 RESOURCE_NOT_FOUND 回执允许变体回退/缺失计数；
// 此处不独立认证归档存在性。其它读取或解析失败保留原诊断前缀并拒绝整批，不发布部分覆盖；
// 仅在该即时失败点追加一条独立Info上下文（上述稳定code、实际请求path），Bake/前置失败不追加。
[[nodiscard]] Result<TerrainCliffs> BuildTerrainCliffs(const MapData::TerrainGridSnapshot& terrain,
    std::span<const std::optional<CliffTypeSource>> cliffTypes, const ModelFileReader& read);

// 坡道边（CliffTrans）：坡道一侧的两个角带坡道标记、另一侧不带，模型覆盖沿坡道方向的两格——
// 坡道角在东/西列时为 1×2（局部 x∈[−128,0]、y∈[0,256]），在南/北行时为 2×1（x∈[−256,0]、y∈[0,128]）。
// 字母取两格外沿四角（西南、西北、东北、东南）：坡道角按相对最低层级记 L/H/X，其余记 A/B/C；
// 找不到匹配的坡道模型时按普通悬崖处理。

// 游戏中坡道中间一排顶点位于两端层级的正中：沿某一轴两侧邻点都是坡道且层级不同时，
// 把该坡道顶点抬到两端层级的平均值（通常 +64）。原始快照不含此调整；渲染、拾取与悬崖使用
// 调整后的副本。返回调整的顶点数，并更新网格的高度边界。
std::size_t ApplyRampHeights(MapData::TerrainGridSnapshot& terrain);

// 模型文件名中的四个层级字母（西南、西北、东北、东南）；差值超过 2 时返回空。
[[nodiscard]] std::optional<std::string> CliffShapeName(std::uint8_t southWest, std::uint8_t northWest,
    std::uint8_t northEast, std::uint8_t southEast);

} // namespace War3Map::Render
