#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

namespace War3Map::Render {

// 宿主把此身份与同一次读取/解析得到的 MapTerrain 一起捕获并持有。
// documentId 为会话内稳定的不透明 ID（1..128 字节，不含 NUL）；generation 区分重新打开。
// sourceSha256 为源归档/目录清单身份，terrainSha256 为实际覆盖解析后 W3E 字节身份。
// SHA 使用 64 位小写十六进制；本查询核对身份，不重新读取来源或计算 SHA。
struct TerrainInspectionBinding {
    std::string documentId;
    std::uint64_t documentGeneration = 0;
    std::uint64_t revision = 0; // 内容修订；刚打开时允许为 0
    std::string sourceSha256;
    std::string terrainSha256;

    [[nodiscard]] bool operator==(const TerrainInspectionBinding&) const noexcept = default;
};

struct TerrainTileRectangle {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct TerrainInspectionRequest {
    TerrainInspectionBinding expectedBinding;
    // 0 基原始 tile 坐标；西南为原点，X 向东、Y 向北，范围是半开区间。
    // 必须非空且完整落在 [0, header.width-1) × [0, header.height-1)。
    TerrainTileRectangle tiles;
};

struct TerrainInspectionLimits {
    // 默认值也是硬上限，只能收紧。不截断、不自动降采样，也不返回部分区块。
    std::size_t maxTiles = 4096;
    std::size_t maxVertices = 8192;
    // 逻辑结果载荷：sizeof(Block)、动态数组元素、字符串字符和终止符。
    // 不含已有 MapTerrain、分配器管理/容量开销、序列化 JSON 和调用者持有的请求。
    std::size_t maxOutputBytes = 2 * 1024 * 1024;
};

struct TerrainInspectionVertex {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t sourceIndex = 0; // y * sourceHeader.width + x
    MapData::TerrainVertex sample; // 原 packed 字节、未知水位标志、世界 XYZ 和基础水位全部保留
};

struct TerrainInspectionTile {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t sourceIndex = 0; // y * (sourceHeader.width-1) + x
    MapData::TerrainTile source; // 四角原始全局顶点索引：SW、SE、NW、NE
    std::array<std::uint32_t, 4> localVertices{}; // 同序，索引到本结果的 vertices
};

struct TerrainInspectionBlock {
    TerrainInspectionBinding binding;
    MapData::TerrainHeader sourceHeader; // width/height 仍是全图顶点数，rawcode 原四字节
    TerrainTileRectangle tiles;
    // 顶点矩形是 (tiles.width+1) × (tiles.height+1)，含东/北边界；两数组均 row-major。
    std::vector<TerrainInspectionVertex> vertices;
    std::vector<TerrainInspectionTile> cells;
    std::array<float, 3> minimum{}; // 本区块顶点的 ground AABB，不含水面或实体
    std::array<float, 3> maximum{};
    std::size_t unknownWaterFlagVertices = 0;
    std::size_t unknownTextureFlagVertices = 0;
    std::size_t unresolvedGroundTextureVertices = 0;
    std::size_t unresolvedCliffTextureVertices = 0; // cliff index 15 保留哨兵不算未知索引
    std::size_t outputBytes = 0; // 按 TerrainInspectionLimits 上述口径计算
};

// Qt/渲染器无关的有界只读查询。只访问请求区块，不重解码、不复制完整网格、不修改修订。
// currentBinding 必须来自可信宿主捕获，不能直接采用客户端请求中的 expectedBinding。
// 函数不会认证 SHA 与 MapTerrain 的对应关系；宿主必须保证该绑定并在返回前复核活动 generation。
// 本结果是 W3E 原始层高网格。预览三角为 (SW,SE,NW)/(SE,NE,NW)，不是原版悬崖/坡道/水面。
[[nodiscard]] Result<TerrainInspectionBlock> QueryTerrainBlock(
    const MapData::MapTerrain& terrain, const TerrainInspectionBinding& currentBinding,
    const TerrainInspectionRequest& request, const TerrainInspectionLimits& limits = {});

} // namespace War3Map::Render
