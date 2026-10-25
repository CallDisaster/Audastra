#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <functional>

namespace War3Map::MapData {

// 输入总量、累计CPU访问和解码驻留分别计费；这些默认值也是硬上限，只能收紧。
// 原字由调用者持有且调用期间不可改写。扫描不复制原字，也不保留完整顶点/tile数组。
struct TerrainRowScanLimits {
    std::size_t maxInputBytes = 4u * 1024u * 1024u;
    std::size_t maxVertexDecodes = 1024u * 1024u;
    std::size_t maxTileAnchors = 1024u * 1024u;
    std::size_t maxWorkUnits = 1024u * 1024u * 1024u;
    std::size_t maxResidentBytes = 2u * 1024u * 1024u;
    [[nodiscard]] bool operator==(const TerrainRowScanLimits&) const = default;
};

struct TerrainRowScanFacts {
    std::size_t inputBytes = 0, vertexDecodes = 0, tileAnchors = 0;
    std::size_t rowVertexMoves = 0, consumerWorkUnits = 0, workUnits = 0;
    std::size_t maximumResidentBytes = 0, maximumResidentRows = 0;
};

[[nodiscard]] Status ValidateTerrainRowScanLimits(const TerrainRowScanLimits& limits);

// 全次扫描共用一份单调累计工作账；消费者在查询邻域前预收其保守上界，不按行重置。
class TerrainRowScanBudget {
public:
    [[nodiscard]] Status ChargeConsumerWork(std::size_t units);
private:
    explicit TerrainRowScanBudget(std::size_t maximum) : maximum_(maximum) {}
    std::size_t maximum_ = 0;
    TerrainRowScanFacts facts_;
    friend Result<TerrainRowScanFacts> ScanTerrainRows(const class TerrainRowSource&,
        const std::function<Status(const TerrainGridSnapshot&, std::uint32_t, std::uint32_t, TerrainRowScanBudget&)>&);
};

class TerrainRowSource {
public:
    TerrainRowSource(TerrainRowSource&&) noexcept = default;
    TerrainRowSource& operator=(TerrainRowSource&&) noexcept = default;
    TerrainRowSource(const TerrainRowSource&) = delete;
    TerrainRowSource& operator=(const TerrainRowSource&) = delete;
    [[nodiscard]] const TerrainHeader& Header() const noexcept { return header_; }
    [[nodiscard]] std::size_t VertexCount() const noexcept { return vertexCount_; }
    [[nodiscard]] std::size_t TileCount() const noexcept { return tileCount_; }
    [[nodiscard]] std::size_t ResidentReservationBytes() const noexcept { return residentBytes_; }
private:
    TerrainRowSource() = default;
    std::span<const std::byte> bytes_;
    TerrainHeader header_;
    TerrainRowScanLimits limits_;
    TerrainReadLimits readLimits_;
    std::size_t recordsOffset_ = 0, vertexCount_ = 0, tileCount_ = 0, residentBytes_ = 0;
    friend Result<TerrainRowSource> OpenTerrainRowSource(std::span<const std::byte>, const TerrainRowScanLimits&);
    friend Result<TerrainRowScanFacts> ScanTerrainRows(const TerrainRowSource&,
        const std::function<Status(const TerrainGridSnapshot&, std::uint32_t, std::uint32_t, TerrainRowScanBudget&)>&);
};

// 与ParseMapTerrain共用同一header/vertex解码及有限坐标/长度/版本检查。
// Open只验证布局并预收最多7行的容量；完整顶点验证由Scan完成，Open成功不是地形有效证明。
[[nodiscard]] Result<TerrainRowSource> OpenTerrainRowSource(std::span<const std::byte> bytes,
    const TerrainRowScanLimits& limits = {});

// 每个原顶点只解码一次，包括可玩区外和末行/列；每个tile锚点也计入整图累计账。
// 访问者按原y递增收到一行，window.header.height是窗口行数，sourceFirstRow给出原行偏移。
// 窗口覆盖目标行±3，足以复用classic悬崖/坡道规则；真实地图边缘保持截断，不伪造邻点。
// 访问者只在本地候选中写结果；后续顶点或预算失败不返回部分成功，不能在回调里执行I/O。
[[nodiscard]] Result<TerrainRowScanFacts> ScanTerrainRows(const TerrainRowSource& source,
    const std::function<Status(const TerrainGridSnapshot& window, std::uint32_t sourceFirstRow,
        std::uint32_t sourceY, TerrainRowScanBudget& budget)>& visitor);

} // namespace War3Map::MapData
