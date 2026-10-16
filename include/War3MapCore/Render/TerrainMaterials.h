#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Resources/TerrainTable.h"

#include <array>
#include <map>
#include <string_view>

namespace War3Map::Render {

struct TerrainMaterialLimits {
    // 默认值也是硬上限，调用方只能收紧。Resolver 的 read 回调本身仍须有源文件读取限额。
    std::size_t maxTableBytes = 2 * 1024 * 1024;
    std::size_t maxRowsPerTable = 4096;
    std::size_t maxCellsPerTable = 262144;
    std::size_t maxCellBytes = 4096;
    std::size_t maxTilesetEntries = 256;
    std::size_t maxWaterFrames = 256;
    std::size_t maxTextureBytes = 16 * 1024 * 1024;
    std::size_t maxTotalTextureBytes = 128 * 1024 * 1024;
    std::size_t maxTextureDimension = 4096;
    std::size_t maxTotalTexturePixels = 16 * 1024 * 1024;
};

using TerrainMaterialRow = Resources::TerrainTableRow;

struct GroundMaterialDefinition {
    TerrainMaterialRow source;
    std::string texturePath; // dir/file.blp，规范化的相对虚拟路径
};

struct CliffMaterialDefinition {
    TerrainMaterialRow source;
    std::string declaredTexturePath; // texDir/texFile.blp；不猜测 tileset 的资源别名
    std::string cliffModelDirectory;
    std::string rampModelDirectory;
    std::string groundTile;
    std::string upperTile; // 表中的 _ / 空值转为空；其余仍为 rawcode
    std::string cliffClass;
};

struct WaterMaterialDefinition {
    TerrainMaterialRow source;
    std::string texturePrefix; // texFile，帧名为 prefix + 至少两位十进制编号 + .blp
    double heightOffsetTiles = 0;
    double heightOffsetWorld = 0; // height * 128；与 W3E waterBaseHeight 相加
    std::uint32_t frameCount = 0;
    double framesPerSecond = 0;
    double textureOffset = 0; // 原表值；此目录尚未定义其渲染解释
    std::uint32_t cells = 0;
    std::uint32_t alphaMode = 0;
    bool lighting = false;
    bool impassable = false;
    std::array<std::uint8_t, 4> minimapColor{}; // RGBA，保持 0..255
    std::array<std::uint8_t, 4> shallowMinimum{};
    std::array<std::uint8_t, 4> shallowMaximum{};
    std::array<std::uint8_t, 4> deepMinimum{};
    std::array<std::uint8_t, 4> deepMaximum{};
    // 波动、岸线和未来版本的其它列仍在 source.fields；没有默认为零或丢弃。
};

struct TerrainMaterialCatalog {
    std::array<Resources::ResourceLocation, 3> tables; // Terrain / CliffTypes / Water
    std::map<std::string, GroundMaterialDefinition, std::less<>> ground;
    std::map<std::string, CliffMaterialDefinition, std::less<>> cliffs;
    std::map<std::string, WaterMaterialDefinition, std::less<>> water;
};

struct TerrainTextureMetadata {
    Resources::ResourceLocation resource;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t compression = 0;
    std::uint32_t alphaBits = 0;
    // 只验证 BLP1 头、各 mip 范围与必要容器结构；没有解码或验证 JPEG/像素内容。
};

struct GroundMaterial {
    GroundMaterialDefinition definition;
    TerrainTextureMetadata texture;
    std::uint32_t atlasColumns = 0; // 经典布局只接受 4 或 8 列、4 行、方形单元格
    std::uint32_t atlasRows = 4;
    std::uint32_t atlasCellPixels = 0;
};

struct CliffMaterial {
    CliffMaterialDefinition definition;
    TerrainTextureMetadata declaredTexture;
    // true 表示只定位了 SLK 声明纹理，尚未证明原版按 tileset 替换的资源身份。
    bool tilesetSubstitutionUnverified = true;
};

struct WaterMaterial {
    WaterMaterialDefinition definition;
    std::vector<TerrainTextureMetadata> frames; // 全部帧须存在且头部尺寸相同
};

struct TerrainWaterFrame {
    std::uint32_t index = 0;
    Resources::ResourceLocation resource;
    std::string sourceSha256;
    Image::DecodedImage image;
};

struct TerrainMaterialSet {
    // 与 W3E 两张 rawcode 表同序，不重排/去重；重复资源只在本次查询内缓存元数据。
    std::vector<GroundMaterial> ground;
    std::vector<CliffMaterial> cliffs;
    WaterMaterial water; // waterID = mainTileset + "Sha"
};

// 复用严格 SlkCells 和 ResourceResolver。三表/必需列/重复 rawcode/数值/路径异常均定位失败。
// 未知列保留，不合并较低优先级表；调用方切换项目或资源层后重新构造目录。
[[nodiscard]] Result<TerrainMaterialCatalog> LoadTerrainMaterialCatalog(
    const Resources::ResourceResolver& resolver, const TerrainMaterialLimits& limits = {});

// 同序绑定 W3E 悬崖目录；缺行保留 null，行/table 身份冲突拒绝。包含 paired ground 来源，
// UI、模型预览和地面派生共用，不再次访问资源。
[[nodiscard]] Result<std::vector<std::optional<CliffTypeSource>>> ResolveTerrainCliffTypes(
    const TerrainMaterialCatalog& catalog, const MapData::TerrainHeader& terrain);

// 只读经典 v11 的材质查询，失败没有部分结果。没有 shader、GPU、贴图像素或地形写回。
// 悬崖只返回声明纹理并附带未验证提示；未覆盖 blight、岸线模型、cliff mesh 与 Reforged。
[[nodiscard]] Result<TerrainMaterialSet> ResolveTerrainMaterialSet(
    const TerrainMaterialCatalog& catalog, const MapData::TerrainHeader& terrain,
    const Resources::ResourceResolver& resolver, const TerrainMaterialLimits& limits = {});

// 只读取显式指定的一帧及用于核对尺寸的首帧，不由 texRate 猜测播放时钟。
// 只检查和解码 mip0；未消费的其它 mip 不影响显示，不表示整条链完整。SHA 覆盖未修改的全部来源字节。
// 索引越界、来源缺失、mip0 损坏、尺寸不一致或预算超限均失败；不得把失败当成下一帧成功。
[[nodiscard]] Result<TerrainWaterFrame> LoadTerrainWaterFrame(
    const WaterMaterialDefinition& definition, std::uint32_t index,
    const Resources::ResourceResolver& resolver, const TerrainMaterialLimits& limits = {});

} // namespace War3Map::Render
