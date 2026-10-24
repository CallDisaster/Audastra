#pragma once

#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Render/TerrainMaterials.h"

#include <cstddef>
#include <string>
#include <vector>

namespace War3Map::Render {

struct TerrainCliffPreviewLimits {
    // 默认值同时是此入口的硬上限；调用方只能收紧。资源来源的 read 回调仍须有读取限额。
    std::size_t maxTiles = 256 * 256;
    std::size_t maxVertices = 2097152;
    std::size_t maxIndices = 6291456;
    std::size_t maxMeshes = 4096;
    std::size_t maxModelPaths = 8192; // 包括不存在的变体路径
    std::size_t maxModelBytes = 16 * 1024 * 1024;
    std::size_t maxTotalModelBytes = 64 * 1024 * 1024;
    TerrainMaterialLimits materials;
};

struct TerrainCliffModelSource {
    Resources::ResourceLocation resource; // 实际命中的地图/游戏来源及成员
    std::string sha256;                    // 对同一次 Read 得到的 MDX 字节计算
    std::size_t bytes = 0;
};

struct TerrainCliffPreview {
    TerrainCliffs geometry; // 烘焙网格可作为 BuildWorldModelScene 的 baked 参数
    std::size_t candidateTiles = 0; // 四角层级不齐或部分坡道标记，可能放模型的原格
    std::size_t rampAdjustedVertices = 0;
    Resources::ResourceLocation cliffTableSource; // 无候选格时不读表，此项为空
    std::vector<TerrainCliffModelSource> modelSources; // 仅实际读到并参与候选的 MDX
    std::vector<std::string> texturePaths; // 烘焙网格引用；贴图读取/解码仍由场景构建负责
};

// 原始 W3E 快照到经典悬崖/坡道模型几何的只读准备入口。内部复制地形并应用坡道高度修正，
// 复用 BuildTerrainCliffs 的原版形状与摆放规则；不修改输入，也不进行 GPU/地图写入。
// 不存在的模型变体允许向 0 回退；命中的资源损坏、来源歧义、读取错误和预算超限则失败，
// 不得把它们伪装为缺模型。成功结果的未覆盖原格及贴图未验证部分须由调用方明确报告。
// 必须使用来源绑定、有限额的 resolver；调用方负责锁定地图和来源版本并验证最终帧身份。
[[nodiscard]] Result<TerrainCliffPreview> BuildTerrainCliffPreview(
    const MapData::TerrainGridSnapshot& terrain, const Resources::ResourceResolver& resolver,
    const TerrainCliffPreviewLimits& limits = {}, std::optional<std::uint8_t> actualMaximumCliffLevel = std::nullopt);

// 已加载材质目录的入口，供地面/水面/悬崖共享同一次 SLK 读取。
[[nodiscard]] Result<TerrainCliffPreview> BuildTerrainCliffPreview(
    const MapData::TerrainGridSnapshot& terrain, const TerrainMaterialCatalog& catalog,
    const Resources::ResourceResolver& resolver, const TerrainCliffPreviewLimits& limits = {},
    std::optional<std::uint8_t> actualMaximumCliffLevel = std::nullopt);

} // namespace War3Map::Render
