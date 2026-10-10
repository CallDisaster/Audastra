#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <memory>
#include <span>

namespace War3Map::MapData {

struct TerrainReadLimits {
    // 默认值也是硬上限，调用方只能收紧；渲染器可以另设更小的网格上传预算。
    std::size_t maxBytes = 64 * 1024 * 1024;
    std::size_t maxDimension = 2049;
    std::size_t maxVertices = 2049 * 2049;
    std::size_t maxTiles = 2048 * 2048;
    std::size_t maxTilesetIds = 256; // 地面与悬崖表各自的预算；保留不可索引的额外表项
    std::size_t maxDecodedBytes = 256 * 1024 * 1024; // 原字节、顶点、tile 索引与表项载荷，不含分配器管理开销
    double maxAbsoluteCoordinate = 16777216.0;
    double maxAbsoluteHeight = 65536.0;
};

struct TerrainHeader {
    std::int32_t version = 11;
    std::uint8_t mainTileset = 0;
    bool usesCustomTileset = false;
    std::vector<std::string> groundTilesets; // rawcode 原四字节，不改大小写
    std::vector<std::string> cliffTilesets;
    std::uint32_t width = 0; // 顶点数，不是 tile 数
    std::uint32_t height = 0;
    std::array<float, 2> offset{}; // 世界坐标中的西南角；不强制地图居中
    [[nodiscard]] std::size_t VertexRecordBytes() const noexcept { return version == 11 ? 7 : (version == 12 ? 8 : 0); }
};

struct TerrainVertex {
    std::uint16_t groundHeightRaw = 0;
    std::uint16_t waterHeightAndFlagsRaw = 0;
    std::uint8_t textureAndFlagsRaw = 0;
    std::uint8_t variationRaw = 0;
    std::uint8_t cliffAndLayerRaw = 0;
    // 世界单位，Z 向上。ground=(raw-8192)/4 + 128*(cliffLevel-2)。
    // 仅为原始层高网格，不包含坡道插值、悬崖模型变形或渲染三角化调整。
    std::array<float, 3> position{};
    // ((waterRaw & 0x3fff)-8192)/4；Water.slk 的材质高度偏移不属于 W3E，调用方另行应用。
    float waterBaseHeight = 0;
    // v12 的 byte B：低两位为水/边界，高六位未知。不能降为经典记录时静默丢弃。
    std::uint8_t textureFlagsExtensionRaw = 0;
    bool extendedTextureLayout = false;

    [[nodiscard]] std::uint8_t GroundTextureIndex() const noexcept { return static_cast<std::uint8_t>(textureAndFlagsRaw & (extendedTextureLayout ? 0x3f : 0x0f)); }
    [[nodiscard]] std::uint8_t GroundVariation() const noexcept { return static_cast<std::uint8_t>(variationRaw & 0x1f); }
    [[nodiscard]] std::uint8_t CliffVariation() const noexcept { return static_cast<std::uint8_t>(variationRaw >> 5); }
    [[nodiscard]] std::uint8_t CliffTextureIndex() const noexcept { return static_cast<std::uint8_t>(cliffAndLayerRaw >> 4); }
    [[nodiscard]] std::uint8_t CliffLevel() const noexcept { return static_cast<std::uint8_t>(cliffAndLayerRaw & 0x0f); }
    [[nodiscard]] bool IsRamp() const noexcept { return (textureAndFlagsRaw & (extendedTextureLayout ? 0x40 : 0x10)) != 0; }
    [[nodiscard]] bool IsBlight() const noexcept { return (textureAndFlagsRaw & (extendedTextureLayout ? 0x80 : 0x20)) != 0; }
    [[nodiscard]] bool HasWater() const noexcept { return extendedTextureLayout ? (textureFlagsExtensionRaw & 1) != 0 : (textureAndFlagsRaw & 0x40) != 0; }
    [[nodiscard]] bool IsBoundary() const noexcept { return extendedTextureLayout ? (textureFlagsExtensionRaw & 2) != 0 : (textureAndFlagsRaw & 0x80) != 0; }
    [[nodiscard]] std::uint8_t UnknownTextureFlags() const noexcept { return extendedTextureLayout ? static_cast<std::uint8_t>(textureFlagsExtensionRaw & 0xfc) : 0; }
    [[nodiscard]] bool IsMapEdge() const noexcept { return (waterHeightAndFlagsRaw & 0x4000) != 0; }
    [[nodiscard]] std::uint16_t UnknownWaterFlags() const noexcept { return static_cast<std::uint16_t>(waterHeightAndFlagsRaw & 0x8000); }
};

struct TerrainTile {
    // SW、SE、NW、NE；预览可用 (0,1,2)、(1,3,2)，对角线 NW→SE。
    std::array<std::uint32_t, 4> vertices{};
};

struct TerrainGridSnapshot {
    TerrainHeader header;
    static constexpr float tileSize = 128.0f;
    // index = y * width + x，首项为西南角，X 向东、Y 向北；tile 同为 row-major。
    std::vector<TerrainVertex> vertices;
    std::vector<TerrainTile> tiles;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    // 只统计，不猜测缺失纹理。悬崖索引 15 为保留哨兵，不算越界。
    std::size_t unresolvedGroundTextures = 0;
    std::size_t unresolvedCliffTextures = 0;
};

class MapTerrain {
public:
    [[nodiscard]] const std::vector<std::byte>& OriginalBytes() const noexcept { return original_; }
    [[nodiscard]] std::shared_ptr<const TerrainGridSnapshot> Snapshot() const noexcept { return snapshot_; }

private:
    std::vector<std::byte> original_;
    std::shared_ptr<const TerrainGridSnapshot> snapshot_;
    friend Result<MapTerrain> ParseMapTerrain(std::span<const std::byte>, const TerrainReadLimits&);
};

// 完整 W3E v11/v12，只读。按版本读取 7/8 字节记录；v12 不开放编辑或序列化。
// 版本/数量/乘法/内存/有限坐标/截断/尾随数据均检查。
// ground bit15 的有符号解释在公开 loader 间有歧义，当前明确拒绝，不生成猜测高度。
// 原始 water bit15 与 cliff=15 保留；未解析到纹理表的索引只诊断，不丢失原始字段。
// 失败不返回部分地形。此 API 不保证原版坡道、悬崖模型、贴图或水面渲染等价。
[[nodiscard]] Result<MapTerrain> ParseMapTerrain(
    std::span<const std::byte> bytes, const TerrainReadLimits& limits = {});

} // namespace War3Map::MapData
